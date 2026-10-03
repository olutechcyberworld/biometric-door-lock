#!/usr/bin/env python3
"""Static sanity check of the project layout (no ESP-IDF needed): every path the build refers to exists."""
import os, re, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
fw = os.path.join(root, "firmware")
bad = 0
def need(path, why):
    global bad
    if not os.path.exists(path):
        print(f"MISSING {os.path.relpath(path, root)}  ({why})"); bad += 1
cm = "\n".join(l for l in open(os.path.join(fw, "CMakeLists.txt")) if not l.lstrip().startswith("#"))
for m in re.finditer(r"\$\{ESP_WHO\}/([\w/]+)", cm):
    need(os.path.join(fw, "third_party/esp-who/components", m.group(1), "CMakeLists.txt"), "EXTRA_COMPONENT_DIRS")
for dp, _, files in os.walk(os.path.join(fw, "third_party")):
    if "idf_component.yml" in files:
        for line in open(os.path.join(dp, "idf_component.yml")):
            m = re.match(r"\s*path:\s*[\"']?([^\"'\s]+)", line)
            if m:
                need(os.path.normpath(os.path.join(dp, m.group(1), "CMakeLists.txt")), f"path: in {os.path.relpath(dp, root)}")
for f in ["main/CMakeLists.txt", "main/idf_component.yml", "main/app_main.cpp", "main/frame_cap_pipeline.cpp",
          "main/fingerprint_task.cpp", "sdkconfig.defaults", "partitions.csv", "dependencies.lock",
          "components/as608/CMakeLists.txt", "components/kconfig_shim/Kconfig",
          "components/auth_fsm/CMakeLists.txt", "components/relay_control/CMakeLists.txt",
          "components/retry_lockout/CMakeLists.txt",
          "main/auth_task.cpp", "main/face_auth_bridge.cpp"]:
    need(os.path.join(fw, f), "firmware file")
d = open(os.path.join(fw, "sdkconfig.defaults")).read()
m = re.search(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', d)
if m:
    need(os.path.join(fw, m.group(1)), "partition table named in sdkconfig.defaults")
elif "CONFIG_PARTITION_TABLE_CUSTOM=y" in d:
    need(os.path.join(fw, "partitions.csv"), "custom partition table (IDF default file name)")
for bad_token in ("IDF_EXTRA_ACTIONS_PATH", "../../components"):
    if bad_token in cm:
        print(f"STALE reference in CMakeLists.txt: {bad_token}"); bad += 1
print("layout OK" if not bad else f"{bad} problem(s)")
sys.exit(1 if bad else 0)
