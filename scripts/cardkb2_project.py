"""Build the standalone OEM-derived IDF project from the root PlatformIO environment."""
from pathlib import Path

Import("env")

project = Path(env.subst("$PROJECT_DIR")) / "firmware" / "cardkb2"
env.Replace(
    PROJECT_DIR=str(project),
    PROJECT_SRC_DIR=str(project / "main"),
)
env.BoardConfig().update("build.partitions", str(project / "partitions.csv"))
