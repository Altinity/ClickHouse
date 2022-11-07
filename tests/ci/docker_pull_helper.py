#!/usr/bin/env python3

import os
import json
import time
import subprocess
import logging
import traceback
import platform


class DockerImage:
    def __init__(self, name, version=None):
        self.name = name
        if version is None:
            self.version = "latest"
        else:
            self.version = version

    def __str__(self):
        return f"{self.name}:{self.version}"


def get_images_with_versions(reports_path, required_image, pull=True):
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
    for image_name in required_image:
        docker_image = image_name if isinstance(image_name, DockerImage) else DockerImage(image_name)
        if image_name in images:
            image_version = images[image_name]
            # NOTE(vnemkov): For some reason we can get version as list of versions,
            # in this case choose one that has commit hash and hence is the longest string.
            # E.g. from ['latest-amd64', '0-amd64', '0-473d8f560fc78c6cdaabb960a537ca5ab49f795f-amd64']
            # choose '0-473d8f560fc78c6cdaabb960a537ca5ab49f795f-amd64' since it 100% points to proper commit.
            if isinstance(image_version, list):
                max_len = 0
                max_len_version = ''
                for version in image_version:
                    if len(version) > max_len:
                        max_len = len(version)
                        max_len_version = version
                logging.debug(f"selected version {max_len_version} from {image_version}")
                image_version = max_len_version

            docker_image.version = image_version
        docker_images.append(docker_image)

    if pull:
        latest_error = None
        for docker_image in docker_images:
            for i in range(10):
                try:
                    logging.info("Pulling image %s", docker_image)
                    latest_error = subprocess.check_output(
                        f"docker pull {docker_image}",
                        stderr=subprocess.STDOUT,
                        shell=True,
                    )
                    break
                except Exception as ex:
                    time.sleep(i * 3)
                    logging.info("Got exception pulling docker %s", ex)
                    latest_error = traceback.format_exc()

                    # # TODO (vnemkov): remove once we have a docker proxy set up.
                    # # Upstream uses some sort of proxy that routes plain images to amd64/aarch64 variants,
                    # # here we do the same manually.
                    # machine_arch = {'x86_64': 'amd64'}[platform.machine().lower()]
                    # if not docker_image.version.endswith(machine_arch):
                    #     docker_image.version = f'{docker_image.version}-{machine_arch}'
                    #     logging.debug('Trying to fetch machine-specific docker image as %s', docker_image)

            else:
                raise Exception(
                    f"Cannot pull dockerhub for image docker pull {docker_image} because of {latest_error}"
                )

    return docker_images


def get_image_with_version(reports_path, image, pull=True):
    return get_images_with_versions(reports_path, [image], pull)[0]

def docker_image(name):
    s = name.split(':')
    return DockerImage(s[0], s[1] if len(s) > 1 else None)

if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)
    def parse_args():
        import argparse
        arg_parser = argparse.ArgumentParser()
        arg_parser.add_argument('--image', type=docker_image, nargs='+')
        arg_parser.add_argument('--pull', type=bool, default=True)
        return arg_parser.parse_args()

    args = parse_args()
    get_images_with_versions('.', args.image, args.pull)
