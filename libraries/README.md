# Custom Libraries Folder

Place reusable ESP-IDF components in this folder for future development.

## Recommended structure

```text
libraries/
  my_component/
    CMakeLists.txt
    include/
      my_component.h
    src/
      my_component.c
```

## Minimal `CMakeLists.txt` example

```cmake
idf_component_register(
    SRCS "src/my_component.c"
    INCLUDE_DIRS "include"
)
```

## Notes

- `project/CMakeLists.txt` sets `EXTRA_COMPONENT_DIRS` to include both `libraries/` and top-level `common/`.
- After adding a new component, run `idf.py reconfigure` or `idf.py build`.
- In other components (such as `project/main`), use:
  - `REQUIRES my_component` in `idf_component_register(...)`
  - `#include "my_component.h"` in source files
