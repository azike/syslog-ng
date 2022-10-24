#############################################################################
# Copyright (c) 2022 Attila Szakacs <szakacs.attila96@gmail.com>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as published
# by the Free Software Foundation, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#
# As an additional exemption you are allowed to compile & link against the
# OpenSSL libraries as published by the OpenSSL project. See the file
# COPYING for details.
#
#############################################################################
from syslogng import Logger, LogParser
import importlib


class KubernetesAPIEnrichment(LogParser):
    logger = Logger()

    def init(self, options):
        try:
            config = importlib.import_module("kubernetes.config")
            client = importlib.import_module("kubernetes.client")
        except ModuleNotFoundError:
            logger.warning("Unable to import kubernetes libraries for Python, no data enrichment is performed")
            self.has_kubernetes_module = False
            return True

        self.has_kubernetes_module = True

        self.__metadata = {}
        self.__prefix = options["prefix"]


        if options["in_pod"] == "yes":
            config.load_incluster_config()
        else:
            config.load_kube_config()

        self.__client_api = client.CoreV1Api()

        return True

    def add_prefix(self, string):
        return "{}{}".format(self.__prefix, string)

    def __get_cached_pod_metadata(self, namespace_name, pod_name):
        self.logger.debug("Trying to find cached metadata for pod {}".format(pod_name))
        return self.__metadata[namespace_name][pod_name]

    def __query_pod_metadata_from_kubernetes_api_server(self, namespace_name, pod_name):
        self.logger.debug("Trying to query metadata for pod {} from the Kubernetes API".format(pod_name))
        pod = self.__client_api.list_namespaced_pod(namespace_name, field_selector="metadata.name=={}".format(pod_name)).items[0]
        container_status = pod.status.container_statuses[0]

        namespace = self.__metadata.setdefault(namespace_name, {})
        pod_metadata = namespace.setdefault(pod_name, {})

        pod_metadata[self.add_prefix("pod_uuid")] = pod.metadata.uid

        for label_name, label_value in pod.metadata.labels.items():
            pod_metadata[self.add_prefix("labels.{}".format(label_name))] = label_value

        pod_metadata[self.add_prefix("namespace_name")] = namespace_name
        pod_metadata[self.add_prefix("pod_name")] = pod_name

        pod_metadata[self.add_prefix("container_name")] = container_status.name
        pod_metadata[self.add_prefix("container_image")] = container_status.image
        pod_metadata[self.add_prefix("container_hash")] = container_status.image_id.replace("docker-pullable://", "", 1)
        pod_metadata[self.add_prefix("docker_id")] = container_status.container_id.replace("docker://", "", 1)

        return pod_metadata

    def get_pod_metadata(self, namespace_name, pod_name):
        try:
            return self.__get_cached_pod_metadata(namespace_name, pod_name)
        except KeyError:
            return self.__query_pod_metadata_from_kubernetes_api_server(namespace_name, pod_name)

    def parse(self, msg):
        if not self.has_kubernetes_module:
            return True

        namespace_name = msg[self.add_prefix("namespace_name")].decode()
        pod_name = msg[self.add_prefix("pod_name")].decode()

        pod_metadata = self.get_pod_metadata(namespace_name, pod_name)
        for name, value in pod_metadata.items():
            if msg[name] == b"":
                msg[name] = value

        return True
