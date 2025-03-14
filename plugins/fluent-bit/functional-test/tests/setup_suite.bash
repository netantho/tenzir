setup_suite() {
  bats_require_minimum_version 1.8.0

  bats_load_library bats-tenzir
  setup_state_dir
}

teardown_suite() {
  bats_load_library bats-tenzir
  try_remove_state_dir
}

BATS_SUITE_DIRNAME="${BATS_TEST_DIRNAME}"
export BATS_SUITE_DIRNAME

TENZIR_DIR="$(realpath "$(dirname "$(command -v tenzir)")")"
export BATS_LIB_PATH=${BATS_LIB_PATH:+${BATS_LIB_PATH}:}${TENZIR_DIR}/../share/tenzir/functional-test:${BATS_SUITE_DIRNAME}/../../../../tenzir/functional-test

unset "${!TENZIR@}"
# Enable bare mode so settings in ~/.config/tenzir or the build configuration
# have no effect.
export TENZIR_BARE_MODE=1
export TENZIR_PLUGINS="fluent-bit"
