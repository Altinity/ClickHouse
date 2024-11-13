#!/usr/bin/env python3
import logging
from argparse import ArgumentDefaultsHelpFormatter, ArgumentParser, ArgumentTypeError
from pathlib import Path
from typing import Any, Dict, Iterable, List, Literal, Optional, Set, Tuple, Union

from git_helper import TWEAK, Git, get_tags, git_runner, removeprefix

FILE_WITH_VERSION_PATH = "cmake/autogenerated_versions.txt"
CHANGELOG_IN_PATH = "debian/changelog.in"
CHANGELOG_PATH = "debian/changelog"
GENERATED_CONTRIBUTORS = "src/Storages/System/StorageSystemContributors.generated.cpp"

# It has {{ for plain "{"
CONTRIBUTORS_TEMPLATE = """// autogenerated by {executer}
const char * auto_contributors[] {{
{contributors}
    nullptr}};
"""

VERSIONS = Dict[str, Union[int, str]]

VERSIONS_TEMPLATE = """# This variables autochanged by tests/ci/version_helper.py:

# NOTE: VERSION_REVISION has nothing common with DBMS_TCP_PROTOCOL_VERSION,
# only DBMS_TCP_PROTOCOL_VERSION should be incremented on protocol changes.
SET(VERSION_REVISION {revision})
SET(VERSION_MAJOR {major})
SET(VERSION_MINOR {minor})
SET(VERSION_PATCH {patch})
SET(VERSION_TWEAK {tweak})
SET(VERSION_FLAVOUR {flavour})
SET(VERSION_GITHASH {githash})
SET(VERSION_DESCRIBE {describe})
SET(VERSION_STRING {string})
# end of autochange
"""


class ClickHouseVersion:
    """Immutable version class. On update returns a new instance"""

    PART_TYPE = Literal["major", "minor", "patch"]

    def __init__(
        self,
        major: Union[int, str],
        minor: Union[int, str],
        patch: Union[int, str],
        revision: Union[int, str],
        git: Optional[Git],
        tweak: Optional[Union[int, str]] = None,
        flavour: Optional[str] = None,
    ):
        self._major = int(major)
        self._minor = int(minor)
        self._patch = int(patch)
        self._revision = int(revision)
        self._git = git
        self._tweak = TWEAK
        if tweak is not None:
            self._tweak = int(tweak)
        elif self._git is not None:
            self._tweak = self._git.tweak
        self._describe = ""
        self._description = ""
        self._flavour = flavour

    def update(self, part: PART_TYPE) -> "ClickHouseVersion":
        """If part is valid, returns a new version"""
        if part == "major":
            return self.major_update()
        if part == "minor":
            return self.minor_update()
        if part == "patch":
            return self.patch_update()
        raise KeyError(f"wrong part {part} is used")

    def bump(self) -> "ClickHouseVersion":
        if self.minor < 12:
            self._minor += 1
            self._revision += 1
            self._patch = 1
            self._tweak = 1
        else:
            self._major += 1
            self._revision += 1
            self._patch = 1
            self._tweak = 1
        return self

    def major_update(self) -> "ClickHouseVersion":
        if self._git is not None:
            self._git.update()
        return ClickHouseVersion(self.major + 1, 1, 1, self.revision + 1, self._git)

    def minor_update(self) -> "ClickHouseVersion":
        if self._git is not None:
            self._git.update()
        return ClickHouseVersion(
            self.major, self.minor + 1, 1, self.revision + 1, self._git
        )

    def patch_update(self) -> "ClickHouseVersion":
        if self._git is not None:
            self._git.update()
        return ClickHouseVersion(
            self.major, self.minor, self.patch + 1, self.revision, self._git
        )

    def reset_tweak(self) -> "ClickHouseVersion":
        if self._git is not None:
            self._git.update()
        return ClickHouseVersion(
            self.major, self.minor, self.patch, self.revision, self._git, 1
        )

    @property
    def major(self) -> int:
        return self._major

    @property
    def minor(self) -> int:
        return self._minor

    @property
    def patch(self) -> int:
        return self._patch

    @property
    def tweak(self) -> int:
        return self._tweak

    @tweak.setter
    def tweak(self, tweak: int) -> None:
        self._tweak = tweak

    @property
    def revision(self) -> int:
        return self._revision

    @property
    def githash(self) -> str:
        "returns the CURRENT git SHA1"
        if self._git is not None:
            return self._git.sha
        return "0000000000000000000000000000000000000000"

    @property
    def describe(self):
        return self._describe

    @property
    def description(self) -> str:
        return self._description

    @property
    def string(self):
        version_as_string = ".".join(
            (str(self.major), str(self.minor), str(self.patch), str(self.tweak))
        )
        if self._flavour:
            version_as_string = f"{version_as_string}.{self._flavour}"
        return version_as_string

    @property
    def is_lts(self) -> bool:
        """our X.3 and X.8 are LTS"""
        return self.minor % 5 == 3

    def get_stable_release_type(self) -> str:
        if self.is_lts:
            return VersionType.LTS
        return VersionType.STABLE

    def as_dict(self) -> VERSIONS:
        return {
            "revision": self.revision,
            "major": self.major,
            "minor": self.minor,
            "patch": self.patch,
            "githash": self.githash,
            "describe": self.describe,
            "string": self.string,
            "tweak": self._tweak or "",
            "flavour": self._flavour or "",
        }

    def as_tuple(self) -> Tuple[int, int, int, int]:
        return (self.major, self.minor, self.patch, self.tweak)

    def with_description(self, version_type):
        if version_type not in VersionType.VALID:
            raise ValueError(f"version type {version_type} not in {VersionType.VALID}")
        self._description = version_type
        if version_type == self._flavour:
            self._describe = f"v{self.string}"
        else:
            self._describe = f"v{self.string}-{version_type}"
        return self

    def copy(self) -> "ClickHouseVersion":
        copy = ClickHouseVersion(
            self.major,
            self.minor,
            self.patch,
            self.revision,
            self._git,
            self.tweak,
        )
        try:
            copy.with_description(self.description)
        except ValueError:
            pass
        return copy

    def __eq__(self, other: Any) -> bool:
        if not isinstance(self, type(other)):
            return NotImplemented
        return bool(
            self.major == other.major
            and self.minor == other.minor
            and self.patch == other.patch
            and self.tweak == other.tweak
        )

    def __lt__(self, other: Any) -> bool:
        if not isinstance(self, type(other)):
            return NotImplemented
        for part in ("major", "minor", "patch", "tweak"):
            if getattr(self, part) < getattr(other, part):
                return True
            elif getattr(self, part) > getattr(other, part):
                return False

        return False

    def __le__(self, other: "ClickHouseVersion") -> bool:
        return self == other or self < other

    def __hash__(self):
        return hash(self.__repr__)

    def __str__(self):
        return f"{self.string}"

    def __repr__(self):
        return (
            f"<ClickHouseVersion({self.major},{self.minor},{self.patch},{self.tweak},"
            f"'{self.description}')>"
        )


ClickHouseVersions = List[ClickHouseVersion]


class VersionType:
    LTS = "lts"
    NEW = "new"
    PRESTABLE = "prestable"
    STABLE = "altinitystable"
    TESTING = "testing"
    VALID = (NEW, TESTING, PRESTABLE, STABLE, LTS,
             "stable" # NOTE (vnemkov): we don't use that directly, but it is used in unit-tests
            )


def validate_version(version: str) -> None:
    # NOTE(vnemkov): minor but imporant fixes, so versions with 'flavour' are threated as valid (e.g. 22.8.8.4.altinitystable)
    parts = version.split(".")
    if len(parts) < 4:
        raise ValueError(f"{version} does not contain 4 parts")
    for part in parts[:4]:
        int(part)


def get_abs_path(path: Union[Path, str]) -> Path:
    return (Path(git_runner.cwd) / path).absolute()


def read_versions(versions_path: Union[Path, str] = FILE_WITH_VERSION_PATH) -> VERSIONS:
    versions = {}
    for line in get_abs_path(versions_path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("SET("):
            continue

        value = 0  # type: Union[int, str]
        name, value = line[4:-1].split(maxsplit=1)
        name = removeprefix(name, "VERSION_").lower()
        try:
            value = int(value)
        except ValueError:
            pass
        versions[name] = value

    return versions


def get_version_from_repo(
    versions_path: Union[Path, str] = FILE_WITH_VERSION_PATH,
    git: Optional[Git] = None,
) -> ClickHouseVersion:
    """Get a ClickHouseVersion from FILE_WITH_VERSION_PATH. When the `git` parameter is
    present, a proper `tweak` version part is calculated for case if the latest tag has
    a `new` type and greater than version in `FILE_WITH_VERSION_PATH`"""
    versions = read_versions(versions_path)
    cmake_version = ClickHouseVersion(
        versions["major"],
        versions["minor"],
        versions["patch"],
        versions["revision"],
        git,
        # Explicitly use tweak value from version file
        tweak=versions.get("tweak", versions["revision"]),
        flavour=versions.get("flavour", None)
    )
    # Since 24.5 we have tags like v24.6.1.1-new, and we must check if the release
    # branch already has it's own commit. It's necessary for a proper tweak version
    if git is not None and git.latest_tag:
        version_from_tag = get_version_from_tag(git.latest_tag)
        if (
            version_from_tag.description == VersionType.NEW
            and cmake_version < version_from_tag
        ):
            # We are in a new release branch without existing release.
            # We should change the tweak version to a `tweak_to_new`
            cmake_version.tweak = git.tweak_to_new
    return cmake_version


def get_version_from_string(
    version: str, git: Optional[Git] = None
) -> ClickHouseVersion:
    validate_version(version)
    # dict for simple handling of missing parts with parts.get(index, default)
    parts = dict(enumerate(version.split(".")))
    return ClickHouseVersion(
        parts[0],
        parts[1],
        parts[2],
        -1,
        git,
        parts.get(3, None),
        parts.get(4, None)
    )


def get_version_from_tag(tag: str) -> ClickHouseVersion:
    Git.check_tag(tag)
    tag, description = tag[1:].split("-", 1)
    version = get_version_from_string(tag)
    version.with_description(description)
    return version


def version_arg(version: str) -> ClickHouseVersion:
    version = removeprefix(version, "refs/tags/")
    try:
        return get_version_from_string(version)
    except ValueError:
        pass
    try:
        return get_version_from_tag(version)
    except ValueError:
        pass

    raise ArgumentTypeError(f"version {version} does not match tag of plain version")


def get_tagged_versions() -> ClickHouseVersions:
    versions = []
    for tag in get_tags():
        try:
            version = get_version_from_tag(tag)
            versions.append(version)
        except Exception:
            continue
    return sorted(versions)


def get_supported_versions(
    versions: Optional[Iterable[ClickHouseVersion]] = None,
) -> Set[ClickHouseVersion]:
    supported_stable = set()  # type: Set[ClickHouseVersion]
    supported_lts = set()  # type: Set[ClickHouseVersion]
    if versions:
        versions = list(versions)
    else:
        # checks that repo is not shallow in background
        versions = get_tagged_versions()
    versions.sort()
    versions.reverse()
    for version in versions:
        if len(supported_stable) < 3:
            if not {
                sv
                for sv in supported_stable
                if version.major == sv.major and version.minor == sv.minor
            }:
                supported_stable.add(version)
        if (version.description == VersionType.LTS or version.is_lts) and len(
            supported_lts
        ) < 2:
            if not {
                sv
                for sv in supported_lts
                if version.major == sv.major and version.minor == sv.minor
            }:
                supported_lts.add(version)
        if len(supported_stable) == 3 and len(supported_lts) == 2:
            break
    return supported_lts.union(supported_stable)


def update_cmake_version(
    version: ClickHouseVersion,
    versions_path: Union[Path, str] = FILE_WITH_VERSION_PATH,
) -> None:
    get_abs_path(versions_path).write_text(
        VERSIONS_TEMPLATE.format_map(version.as_dict()), encoding="utf-8"
    )


def update_contributors(
    relative_contributors_path: Union[Path, str] = GENERATED_CONTRIBUTORS,
    force: bool = False,
    raise_error: bool = False,
) -> None:
    # Check if we have shallow checkout by comparing number of lines
    # '--is-shallow-repository' is in git since 2.15, 2017-10-30
    if git_runner.run("git rev-parse --is-shallow-repository") == "true" and not force:
        logging.warning("The repository is shallow, refusing to update contributors")
        if raise_error:
            raise RuntimeError("update_contributors executed on a shallow repository")
        return

    # format: "  1016  Alexey Arno"
    shortlog = git_runner.run("git shortlog HEAD --summary")
    escaping = str.maketrans({"\\": "\\\\", '"': '\\"'})
    contributors = sorted(
        [c.split(maxsplit=1)[-1].translate(escaping) for c in shortlog.split("\n")],
    )
    contributors = [f'    "{c}",' for c in contributors]

    executer = Path(__file__).relative_to(git_runner.cwd)
    content = CONTRIBUTORS_TEMPLATE.format(
        executer=executer, contributors="\n".join(contributors)
    )
    get_abs_path(relative_contributors_path).write_text(content, encoding="utf-8")


def update_version_local(version : ClickHouseVersion, version_type="testing"):
    update_contributors()
    version.with_description(version_type)
    update_cmake_version(version)


def main():
    """The simplest thing it does - reads versions from cmake and produce the
    environment variables that may be sourced in bash scripts"""
    parser = ArgumentParser(
        formatter_class=ArgumentDefaultsHelpFormatter,
        description="The script reads versions from cmake and produce ENV variables",
    )
    parser.add_argument(
        "--version-path",
        "-p",
        default=FILE_WITH_VERSION_PATH,
        help="relative path to the cmake file with versions",
    )
    parser.add_argument(
        "--version-type",
        "-t",
        choices=VersionType.VALID,
        default=VersionType.TESTING,
        help="optional parameter to generate DESCRIBE",
    )
    parser.add_argument(
        "--export",
        "-e",
        action="store_true",
        help="if the ENV variables should be exported",
    )
    parser.add_argument(
        "--update-part",
        choices=("major", "minor", "patch"),
        help="the version part to update, tweak is always calculated from commits, "
        "implies `--update-cmake`",
    )
    parser.add_argument(
        "--update-cmake",
        "-u",
        action="store_true",
        help=f"is update for {FILE_WITH_VERSION_PATH} is needed or not",
    )
    parser.add_argument(
        "--update-contributors",
        "-c",
        action="store_true",
        help=f"update {GENERATED_CONTRIBUTORS} file and exit, "
        "doesn't work on shallow repo",
    )
    args = parser.parse_args()

    if args.update_contributors:
        update_contributors()
        return

    version = get_version_from_repo(args.version_path, Git(True))

    if args.update_part:
        version = version.update(args.update_part)

    version.with_description(args.version_type)

    if args.update_part or args.update_cmake:
        update_cmake_version(version)

    for k, v in version.as_dict().items():
        name = f"CLICKHOUSE_VERSION_{k.upper()}"
        print(f"{name}='{v}'")
        if args.export:
            print(f"export {name}")


if __name__ == "__main__":
    main()
