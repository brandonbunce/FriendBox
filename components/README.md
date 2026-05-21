# IDF components

Vendored third-party ESP-IDF components. Each is added as a git submodule so we
can pin a known-good revision without bloating the main repo. Run these once
after cloning the repo:

```sh
git submodule add -b master https://github.com/lovyan03/LovyanGFX.git components/LovyanGFX
git -C components/LovyanGFX checkout 1.2.7

git submodule add https://github.com/bblanchon/ArduinoJson.git components/ArduinoJson
git -C components/ArduinoJson checkout v7.4.2
```

Both projects ship ESP-IDF support natively:

- **LovyanGFX** has an `idf_component.yml` / `CMakeLists.txt` declaring the
  `lovyangfx` component. `Bus_SPI` and `Touch_GT911` use `driver/spi_master.h`
  and `driver/i2c.h` directly under IDF.
- **ArduinoJson** is header-only with an `idf_component.yml`. Once cloned, the
  component is just `INCLUDE_DIRS=src`.

After adding the submodules, declare the dependencies in `src/CMakeLists.txt`
under `REQUIRES` (the names are component names from each lib's
`idf_component.yml`):

```cmake
set(REQUIRES
    ...
    lovyangfx
    ArduinoJson
)
```
