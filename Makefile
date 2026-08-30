.PHONY: default build sim test all format format-check lint clean

# Each devShell sets its own BUILD_DIR so toolchains never share a cmake cache.
BUILD_DIR ?= build

default:
	@echo "Cosmos build tasks:"
	@echo "  make build         - Build production binaries (Release)"
	@echo "  make sim           - Build simulation binaries only"
	@echo "  make test          - Build and run unit tests"
	@echo "  make all           - Build production binaries, examples, and tests"
	@echo "  make format        - Format code with clang-format"
	@echo "  make format-check  - Verify code formatting without modifying files"
	@echo "  make lint          - Lint and verify code formatting"
	@echo "  make clean         - Remove build directory"

build:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --parallel --target kv_store_prod replicated_kv_prod

sim:
	cmake -B $(BUILD_DIR) -DCOSMOS_BUILD_TESTS=OFF
	cmake --build $(BUILD_DIR) --parallel --target kv_store_sim replicated_kv_sim

test:
	cmake -B $(BUILD_DIR) -DCOSMOS_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR) --parallel
	ctest --test-dir $(BUILD_DIR) --output-on-failure

all:
	cmake -B $(BUILD_DIR) -DCOSMOS_BUILD_TESTS=ON
	cmake --build $(BUILD_DIR) --parallel

format:
	find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +

format-check:
	find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run --Werror {} +

lint: format-check

clean:
	rm -rf $(BUILD_DIR)
