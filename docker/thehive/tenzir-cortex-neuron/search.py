#!/usr/bin/env python3
# -*- coding: utf-8 -*

import asyncio
import json
from cortexutils.analyzer import Analyzer
from pytenzir import Tenzir


class VastAnalyzer(Analyzer):
    def __init__(self):
        Analyzer.__init__(self)
        self.host = self.get_param(
            "config.endpoint", None, "Vast Server endpoint is missing"
        )
        self.max_events = self.get_param(
            "config.max_events", 30, "Max result value is missing"
        )
        self.tenzirClient = Tenzir(endpoint=self.host)

    async def run(self):
        try:
            if not await self.tenzirClient.test_connection():
                self.error("Could not connect to Tenzir Server Endpoint")

            query = ""
            if self.data_type == "ip":
                query = f":ip == {self.get_data()}"
            elif self.data_type == "subnet":
                query = f":ip in {self.get_data()}"
            elif self.data_type in ["hash", "domain"]:
                query = f"{self.get_data()}"

            proc = (
                await self.tenzirClient.export(max_events=self.max_events)
                .json(query)
                .exec()
            )
            stdout, stderr = await proc.communicate()
            if stdout != b"":
                result = [
                    json.loads(str(item))
                    for item in stdout.decode("ASCII").strip().split("\n")
                ]
            else:
                result = []

            self.report({"values": result})
        except Exception as e:
            error = {"input": self._input, "error": e}
            self.unexpectedError(error)

    def summary(self, raw):
        taxonomies = []
        namespace = "Tenzir"
        predicate = "Hits"

        valuesCount = len(raw["values"])
        value = f"{valuesCount}"
        if valuesCount > 0:
            level = "suspicious"
        else:
            level = "safe"

        taxonomies.append(self.build_taxonomy(level, namespace, predicate, value))

        return {"taxonomies": taxonomies}


if __name__ == "__main__":
    asyncio.run(VastAnalyzer().run())
