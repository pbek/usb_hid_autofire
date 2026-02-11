_:

{
  enterShell = ''
    echo "🛠️ usb_hid_autofire dev shell"
  '';

  git-hooks.hooks = {
    clang-format = {
      enable = true;
    };
  };

  # See full reference at https://devenv.sh/reference/options/
}
