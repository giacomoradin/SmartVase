# Doxygen theme (doxygen-awesome-css)

The HTML site uses [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css)
**pinned at version `v2.3.4`** as its theme, with overrides in [`custom.css`](custom.css)
(SmartVase green palette).

- `custom.css` is **committed** (it's ours).
- `doxygen-awesome.css` is **not** committed (it's third-party) and is in `.gitignore`:
  - in **CI**, the GitHub Action downloads it at the pinned version before running Doxygen;
  - **locally**, download it once (needs network access) with:

    ```bash
    curl -L -o docs/doxygen-theme/doxygen-awesome.css \
      https://raw.githubusercontent.com/jothepro/doxygen-awesome-css/v2.3.4/doxygen-awesome.css
    ```

If the file is not present, Doxygen still generates the docs using the default
theme (it only emits a "stylesheet not found" warning): the theme is an extra, not a requirement.

> To update the theme version: change `v2.3.4` here, in
> `.github/workflows/generate-docs.yml` and (if you want) re-vendor it locally.
