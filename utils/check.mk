# https://github.com/rizsotto/Bear
compile_commands.json:
	@mkdir -p $(@D)
	bear -- $(MAKE) clean all TOOLCHAIN=llvm

.PHONY: check_analyze
check_analyze: compile_commands.json
	CodeChecker analyze $< -o bin/reports --ctu

.PHONY: check_html
check_html: check_analyze
	CodeChecker parse bin/reports -e html -o bin/reports_html
