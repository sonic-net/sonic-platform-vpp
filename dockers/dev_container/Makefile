docker_run:
	@echo "Running Docker container..."
	chmod +x run.sh 
	./run.sh
build_container:
	@echo "Executing script..."
	chmod +x build.sh 
	./build.sh

copy_vpplib:
	@echo "Copying folder..."
	cp -r  $(HOME)/workspace/old/build/sonic-buildimage/src/sonic-sairedis $(HOME)/workspace/new/build/sonic-buildimage/src/sonic-sairedis
