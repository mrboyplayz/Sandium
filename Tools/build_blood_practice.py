"""Package the native blood Practice addon."""

from pathlib import Path
import json
import shutil

root = Path(__file__).resolve().parents[1]
addon = root / "Tools/practice-blood-addon"
(addon / "client").mkdir(parents=True, exist_ok=True)
(addon / "addon.json").write_text(json.dumps({
    "id": "blood_particles",
    "name": "Native Blood",
    "description": "Sub Rosa blood hit effects and floor impacts in Practice Mode.",
    "requires": [],
    "conflicts": [],
    "disabled": False,
}, indent=2) + "\n")
shutil.copy2(root / "Tools/practice-native-blood.lua", addon / "client/init.lua")
print(addon)
