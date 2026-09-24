import sys
from pathlib import Path


def main():
    if len(sys.argv) < 2:
        print("Usage: py Tools/CreateReviaClass.py ClassName [Folder]")
        print("Example: py Tools/CreateReviaClass.py ReviaApp Core")
        sys.exit(1)

    class_name = sys.argv[1]
    folder_name = sys.argv[2] if len(sys.argv) >= 3 else ""

    root = Path(__file__).resolve().parents[1]

    public_path = root / "Public"
    private_path = root / "Private"

    if folder_name:
        public_path = public_path / folder_name
        private_path = private_path / folder_name

    header_file = public_path / f"{class_name}.h"
    source_file = private_path / f"{class_name}.cpp"

    print(f"Project root: {root}")
    print(f"Public path:  {public_path}")
    print(f"Private path: {private_path}")
    print(f"Header file:  {header_file}")
    print(f"Source file:  {source_file}")

    public_path.mkdir(parents=True, exist_ok=True)
    private_path.mkdir(parents=True, exist_ok=True)

    if header_file.exists() or source_file.exists():
        print(f"Error: {class_name} already exists.")
        sys.exit(1)

    include_path = f"{folder_name}/{class_name}.h" if folder_name else f"{class_name}.h"

    header_file.write_text(f"""#pragma once

class {class_name}
{{
public:
    {class_name}();
    ~{class_name}();

private:

}};
""", encoding="utf-8")

    source_file.write_text(f"""#include "{include_path}"

{class_name}::{class_name}()
{{
}}

{class_name}::~{class_name}()
{{
}}
""", encoding="utf-8")

    print("Created:")
    print(f"  {header_file}")
    print(f"  {source_file}")


if __name__ == "__main__":
    main()