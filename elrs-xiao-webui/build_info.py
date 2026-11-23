# PlatformIO pre-script: the release this firmware is (FW_VERSION: the
# release branch CI builds, else the nearest release tag) and its git
# commit (FW_BUILD), shown on the update page and used by the online update.
Import("env")  # noqa: F821  (provided by PlatformIO)
import os
import sys

project = env.subst("$PROJECT_DIR")  # noqa: F821
root = os.path.dirname(project)
sys.path.insert(0, os.path.join(root, "tools"))
from build_id import build_id, release_version  # noqa: E402

bid = build_id(root)
ver = release_version(root)
env.Append(CPPDEFINES=[("FW_BUILD", env.StringifyMacro(bid)),  # noqa: F821
                       ("FW_VERSION", env.StringifyMacro(ver))])  # noqa: F821
print("build_info.py: FW_BUILD = %s, FW_VERSION = %s" % (bid, ver))
