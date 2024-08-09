#!/usr/bin/env python3

import os
import json
import time
import subprocess
import logging
import traceback

from pathlib import Path
from typing import List, Optional, Union


class DockerImage:
    def __init__(self, name: str, version: Optional[str] = None):
        self.name = name
        if version is None:
            self.version = "latest"
        else:
            self.version = version

    def __str__(self):
        return f"{self.name}:{self.version}"


def get_images_with_versions(
    reports_path: Union[Path, str],
    required_images: List[str],
    pull: bool = True,
    version: Optional[str] = None,
) -> List[DockerImage]:
    images_path = None
    for root, _, files in os.walk(reports_path):
        for f in files:
            if f == "changed_images.json":
                images_path = os.path.join(root, "changed_images.json")
                break

    if not images_path:
        logging.info("Images file not found")
    else:
        logging.info("Images file path %s", images_path)

    if images_path is not None and os.path.exists(images_path):
        logging.info("Images file exists")
        with open(images_path, "r", encoding="utf-8") as images_fd:
            images = json.load(images_fd)
            logging.info("Got images %s", images)
    else:
        images = {}

    docker_images = []
    for image_name in required_images:
        docker_image = DockerImage(image_name, version)
        if image_name in images:
            image_version = images[image_name]
            # NOTE(vnemkov): For some reason we can get version as list of versions,
            # in this case choose one that has commit hash and hence is the longest string.
            # E.g. from ['latest-amd64', '0-amd64', '0-473d8f560fc78c6cdaabb960a537ca5ab49f795f-amd64']
            # choose '0-473d8f560fc78c6cdaabb960a537ca5ab49f795f-amd64' since it 100% points to proper commit.
            if isinstance(image_version, list):
                max_len = 0
                max_len_version = ''
                for version_variant in image_version:
                    if len(version_variant) > max_len:
                        max_len = len(version_variant)
                        max_len_version = version_variant
                logging.debug(f"selected version {max_len_version} from {image_version}")
                image_version = max_len_version

            docker_image.version = image_version

        docker_images.append(docker_image)

    latest_error = Exception("predefined to avoid access before created")
    if pull:
        latest_error = None
        for docker_image in docker_images:
            for i in range(10):
                try:
                    logging.info("Pulling image %s", docker_image)
                    subprocess.check_output(
                        f"docker pull {docker_image}",
                        stderr=subprocess.STDOUT,
                        shell=True,
                    )
                    break
                except Exception as ex:
                    latest_error = ex
                    time.sleep(i * 3)
                    logging.info("Got exception pulling docker %s", ex)
                    latest_error = traceback.format_exc()
            else:
                raise Exception(
                    "Cannot pull dockerhub for image docker pull "
                    f"{docker_image} because of {latest_error}"
                )

    return docker_images


def get_image_with_version(
    reports_path: Union[Path, str],
    image: str,
    pull: bool = True,
    version: Optional[str] = None,
) -> DockerImage:
    logging.info("Looking for images file in %s", reports_path)
    return get_images_with_versions(reports_path, [image], pull, version=version)[0]
