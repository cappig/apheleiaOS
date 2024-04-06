.PHONY: docker_image
docker_image:
	docker build utils -t apheleia

.PHONY: docker_build
docker_build:
	docker run -it -v "$$PWD":/usr/src/apheleia -w /usr/src/apheleia \
		-u `stat -c "%u:%g" .` apheleia:latest $(MAKE)
