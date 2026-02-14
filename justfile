# Use `just <recipe>` to run a recipe
# https://just.systems/man/en/

import ".shared/common.just"

# By default, run the `--list` command
default:
    @just --list

# Variables

transferDir := `if [ -d "$HOME/NextcloudPrivate/Transfer" ]; then echo "$HOME/NextcloudPrivate/Transfer"; else echo "$HOME/Nextcloud/Transfer"; fi`
projectName := "usb_hid_autofire"
firmwarePath := ".firmware"

# Aliases

# Apply a git patch to the project
[group('patches')]
git-apply-patch:
    git apply {{ transferDir }}/{{ projectName }}.patch

# Create git patches for the project and some libraries
[group('patches')]
git-create-patch:
    @echo "transferDir: {{ transferDir }}"
    git diff --no-ext-diff --staged --binary > {{ transferDir }}/{{ projectName }}.patch
    ls -l1t {{ transferDir }} | head -2

# Install the firmware and link the project
[group('firmware')]
firmware-install:
    #!/usr/bin/env bash
    if [ -d "{{ firmwarePath }}" ]; then
        echo "Firmware already installed at {{ firmwarePath }}"
        exit 0
    fi
    git clone https://github.com/flipperdevices/flipperzero-firmware.git --recursive --depth 1 {{ firmwarePath }}
    cd {{ firmwarePath }}
    ./fbt
    cd applications_user
    ln -s ../.. {{ projectName }}

# Remove the firmware
[group('firmware')]
firmware-remove:
    rm -rf {{ firmwarePath }}

# Build the application
[group('dev')]
build:
    #!/usr/bin/env bash
    if [ ! -d "{{ firmwarePath }}" ]; then
        just firmware-install
    fi
    cd {{ firmwarePath }}
    nix run nixpkgs#steam-run --impure -- ./fbt fap_{{ projectName }}

# Launch the application
[group('dev')]
launch:
    #!/usr/bin/env bash
    if [ ! -d "{{ firmwarePath }}" ]; then
        just firmware-install
    fi
    cd {{ firmwarePath }}
    nix run nixpkgs#steam-run --impure -- ./fbt launch APPSRC={{ projectName }}
