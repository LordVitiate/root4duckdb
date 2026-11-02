from configparser import RawConfigParser
from pathlib import Path

fixed = {"baseos": "BaseOS", "appstream": "AppStream"}
replaced = set()
for path in Path("/etc/yum.repos.d").glob("*.repo"):
    config = RawConfigParser(interpolation=None)
    config.read(path)
    changed = False
    for section in config.sections():
        key = section.lower()
        if key in fixed:
            config.set(section, "baseurl", f"https://repo.almalinux.org/almalinux/8.10/{fixed[key]}/aarch64/os/")
            config.remove_option(section, "mirrorlist")
            config.remove_option(section, "metalink")
            replaced.add(key)
            changed = True
        elif key == "epel" or key.startswith("epel-"):
            config.set(section, "enabled", "0")
            changed = True
    if changed:
        with path.open("w") as output:
            config.write(output)
if replaced != fixed.keys():
    raise RuntimeError(f"Missing AlmaLinux 8 repositories: {sorted(set(fixed) - replaced)}")

epel = RawConfigParser(interpolation=None)
epel["root4duckdb-epel"] = {
    "name": "EPEL 8 aarch64",
    "baseurl": "https://mirror.metanet.ch/fedora-epel/8/Everything/aarch64/",
    "enabled": "1",
    "gpgcheck": "1",
    "gpgkey": "https://mirror.metanet.ch/fedora-epel/RPM-GPG-KEY-EPEL-8",
}
with Path("/etc/yum.repos.d/root4duckdb-epel.repo").open("w") as output:
    epel.write(output)
