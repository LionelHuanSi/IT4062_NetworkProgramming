STUDENT_NAME = NguyenVanHuan
STUDENT_ID = 20235733
ZIP_NAME = $(STUDENT_NAME)_$(STUDENT_ID)_HW3.zip

all:
	$(MAKE) -C bai1
	$(MAKE) -C bai2

clean:
	$(MAKE) -C bai1 clean
	$(MAKE) -C bai2 clean
	rm -f $(ZIP_NAME)

zip: clean
	@if command -v zip > /dev/null 2>&1; then \
		zip -r $(ZIP_NAME) Makefile bai1 bai2; \
	else \
		python3 -m zipfile -c $(ZIP_NAME) Makefile bai1 bai2; \
	fi
	@echo "Package created: $(ZIP_NAME)"

.PHONY: all clean zip
