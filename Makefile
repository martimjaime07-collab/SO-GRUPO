all:
	$(MAKE) -C src
	cp -f src/cloudIST cloudIST

clean:
	$(MAKE) -C src clean
	rm -f cloudIST

run: all
	./cloudIST

.PHONY: all clean run
