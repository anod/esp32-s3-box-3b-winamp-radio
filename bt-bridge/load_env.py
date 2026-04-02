Import("env")
import os

# Load .env from parent directory (shared with radio project)
env_file = os.path.join(env.subst("$PROJECT_DIR"), "..", ".env")
if os.path.isfile(env_file):
    with open(env_file) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            if key and value:
                env.Append(CPPDEFINES=[(key, env.StringifyMacro(value))])
                print(f"  .env: {key} = ****")
