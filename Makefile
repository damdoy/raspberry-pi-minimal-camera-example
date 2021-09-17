all:
	gcc main.c -o camera -O2 -I /opt/vc/include/ -I/opt/vc/include/interface/mmal/ -L/opt/vc/lib/ -lmmal_util -lmmal_core -lbcm_host -lmmal_vc_client -Wl,--whole-archive -lmmal_components -Wl,--no-whole-archive -lmmal_core -lpthread
