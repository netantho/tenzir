from pytenzir import Tenzir, ExportMode, collect_pyarrow, to_json_rows, VastRow
import pytenzir.utils.logging
import pytenzir.utils.asyncio
import asyncio
from asyncio.subprocess import PIPE
import os
import pytest
import shutil

logger = pytenzir.utils.logging.get("tenzir.test")

if "TENZIR_PYTHON_INTEGRATION" not in os.environ:
    # Tests in this module require access to integration test files and the Tenzir binary
    pytest.skip(
        "TENZIR_PYTHON_INTEGRATION not defined, skipping tenzir tests",
        allow_module_level=True,
    )


@pytest.fixture()
async def endpoint():
    test = os.environ.get("PYTEST_CURRENT_TEST").split(":")[-1].split(" ")[0]
    test_db_dir = "/tmp/pytenzir-test/" + test
    if os.path.isdir(test_db_dir):
        shutil.rmtree(test_db_dir)
    proc = await asyncio.create_subprocess_exec(
        "tenzir-ctl",
        "-e",
        ":0",
        "-d",
        test_db_dir,
        "start",
        "--print-endpoint",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    endpoint = await proc.stdout.readline()
    endpoint = endpoint.decode("utf-8").strip()
    logger.debug(f"{endpoint = }")
    yield endpoint
    proc.terminate()
    await proc.communicate()
    # TODO: kill if in case of timeout.
    await asyncio.wait_for(proc.wait(), 5)


async def tenzir_import(endpoint, expression: list[str]):
    # import
    logger.debug(f"> tenzir-ctl -e {endpoint} import --blocking {' '.join(expression)}")
    import_proc = await asyncio.create_subprocess_exec(
        "tenzir-ctl", "-e", endpoint, "import", "--blocking", *expression, stderr=PIPE
    )
    (_, import_err) = await asyncio.wait_for(import_proc.communicate(), 3)
    assert import_proc.returncode == 0
    logger.debug(f"tenzir-ctl import stderr:\n{import_err.decode()}")
    # flush
    logger.debug(f"> tenzir-ctl -e {endpoint} flush")
    flush_proc = await asyncio.create_subprocess_exec(
        "tenzir-ctl", "-e", endpoint, "flush", stderr=PIPE
    )
    (_, flush_err) = await asyncio.wait_for(flush_proc.communicate(), 3)
    assert flush_proc.returncode == 0
    logger.debug(f"tenzir-ctl flush stderr:\n{flush_err.decode()}")


def integration_data(path):
    dir_path = os.path.dirname(os.path.realpath(__file__))
    return os.path.normpath(f"{dir_path}/../../tenzir/functional-test/data/{path}")


@pytest.mark.asyncio
async def test_count(endpoint):
    tenzir = Tenzir(endpoint)
    result = await tenzir.count()
    assert result == 0
    await tenzir_import(
        endpoint, ["-r", integration_data("suricata/eve.json"), "suricata"]
    )
    result = await tenzir.count()
    assert result == 8


@pytest.mark.asyncio
async def test_export_collect_pyarrow(endpoint):
    await tenzir_import(
        endpoint, ["-r", integration_data("suricata/eve.json"), "suricata"]
    )
    tenzir = Tenzir(endpoint)
    result = tenzir.export('#schema == "suricata.alert"', ExportMode.HISTORICAL)
    tables = await collect_pyarrow(result)
    assert set(tables.keys()) == {"suricata.alert"}
    alerts = tables["suricata.alert"]
    assert len(alerts) == 1
    assert alerts[0].num_rows == 1

    result = tenzir.export("", ExportMode.HISTORICAL)
    tables = await collect_pyarrow(result)
    assert set(tables.keys()) == {
        "suricata.alert",
        "suricata.dns",
        "suricata.netflow",
        "suricata.flow",
        "suricata.fileinfo",
        "suricata.http",
        "suricata.stats",
        "suricata.quic",
    }


@pytest.mark.asyncio
async def test_export_historical_rows(endpoint):
    await tenzir_import(
        endpoint, ["-r", integration_data("suricata/eve.json"), "suricata"]
    )
    tenzir = Tenzir(endpoint)
    result = tenzir.export('#schema == "suricata.alert"', ExportMode.HISTORICAL)
    rows: list[VastRow] = []
    async for row in to_json_rows(result):
        rows.append(row)
    assert len(rows) == 1
    assert rows[0].name == "suricata.alert"
    # only assert extension types here
    alert = rows[0].data
    assert alert["src_ip"] == "147.32.84.165"
    assert alert["dest_ip"] == "78.40.125.4"


@pytest.mark.asyncio
async def test_export_continuous_rows(endpoint):
    tenzir = Tenzir(endpoint)

    async def run_export():
        result = tenzir.export('#schema == "suricata.alert"', ExportMode.CONTINUOUS)
        return await anext(to_json_rows(result))

    task = asyncio.create_task(run_export())
    # Wait for the export task to be ready before triggering import
    await asyncio.sleep(3)
    await tenzir_import(
        endpoint, ["-r", integration_data("suricata/eve.json"), "suricata"]
    )
    logger.info("await task")
    row = await asyncio.wait_for(task, 5)
    logger.info("task awaited")
    assert row is not None
    assert row.name == "suricata.alert"
    # only assert extension types here
    alert = row.data
    assert alert["src_ip"] == "147.32.84.165"
    assert alert["dest_ip"] == "78.40.125.4"
