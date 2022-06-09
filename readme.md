# VSCode template
- this repo serves as template for vscode STM32 projects, primarily targeting Linux
- files come from TLM project with some modifications

## Linux 
### SW reuirements
- up to date distro (rolling release is advantage)
- vscode/vscodium
  - distribution repository/snap package/github release
  - necessary plugins:
    - Cmake Tools
    - C/C++
    - cortex-debug
- toolchain (GCC arm-none-eabi)
  - distribution repository/[[https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-rm/downloads | arm.com]]
- upstream openocd, stable release is over 3 years old, so pretty much outdated
  - compile from sources (Arch AUR)/[[https://github.com/xpack-dev-tools/openocd-xpack | find some binary build]]
- cmake
  - distribution repository
- ninja build system
  - distribution repository

*following instructions expect all binaries are in PATH*

### What is in this repo
- `.vscode`
  - usually hidden directory with all vscode config relevant to this workspace
  - `tasks.json`
    - contains user definable tasks like build/install/flash/...
    - feel free to experiment (vscode does auto-complete from json config files)
  - `launch.json`
    - contains launch (debug/attach) configurations like binary location, SWO config, etc.
  - `settings.json`
    - contains workspace config, only cmake config by default
  - other config files, workspace specific config files
- `CMakeLists.txt` build configuration (MCU, FPU, linker script file, warnings, ...)
- `STM32[F,L]xx.svd` placeholder for SW file for for your MCU (allows looking into peripheral registers)
  - replace this file with one matching your MCU, adjust filename in `launch.json`
- `STM32[F,L]xx.ld` placeholder linked script
  - replace with your linker script. Or if want to start from scratch, this one should do, you'll have to adjust memory (RAM) addresses and sizes.
- `openocd.cfg`
  - Openocd configuration including target and adapter selection
  - uncomment appropriate line with your MCU
- `.gitignore` (hidden)
  - used to disable some files to be added into repo, usually files produces by build
- `.clang-format`
  - authors' preferred formatting config (generated using [this online tool]https://zed0.co.uk/clang-format-configurator/), feel free to adjust

### How to use
*following text expects working git client on your machine including SSH keys etc. and working all software mentioned above*

- download contents of this repo: `git clone git@eforce1.feld.cvut.cz:fse/vscode_template.git`
- modify all necessary files (linker script, openocd.cfg, launch.json, CMakeLists.txt)
- copy your source into this repo (`src` directory is reasonable)
- add sources into `CMakeLists.txt`, also adjust include paths (this is kinda pain with HAl etc)
- create directory build `mkdir build` and cd there `cd build`
- configure cmake `cmake -GNinja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake ..`

*Now to vscode...*
- open vscode, then `add directory to workspace` and select this cloned repo
- vscode will ask, if you want to configure CMake at launch, say yet for this workspace
- F7 should start build, if it fails, **read error message** and you'll probably know what is wrong, possible errors:
  - wroken code
  - wrong filenames
  - forgot to select/uncomment/configure something
  - ???
- F5 launches debug session, if everything works, you should see startup file and MCU being at the start of reset vector
- **Enjoy debugging!**

#### Ready to commit...
- head to our [gitlab](https://eforce1.feld.cvut.cz/gitlab/) and create new project, resulting address will be something like `https://eforce1.feld.cvut.cz/gitlab/your_user_name/my_awesome_project`
- `cd vscode_template`
- remove this repo history `rm -rf .git`

*these instructions are identical to those given by gitlab after new repo creation*

*(Git global setup)*
- `git config --global user.name "Yout Name"`
- `git config --global user.email "yournameornick@eforce.fel.cvut.cz"`

*(Push an existing folder)*
- `git init`
- `git remote add origin git@eforce1.feld.cvut.cz:your_user_name/your_awesome_project.git`
- `git add .` (if you want to add all files)
- `git commit -m "Initial commit"`
- `git push -u origin master`


## some tips
- Learn Rust or C++, C is just too unsafe
- When using raw IRQ handlers in C++, don't forget to prefix their definitions with `extern "C"` to have C style linkage for startup file (ASM/C)
- understand your MCU and HW (at least schematic)
- if in doubt or stuck on error, don't hesitate to contact other team members on Discord