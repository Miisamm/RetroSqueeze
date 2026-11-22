# Open Watcom 2.0 (wmake -h). Set up its environment first (WATCOM, PATH, INCLUDE=$WATCOM/h:$WATCOM/h/nt).  All objects use the multithreaded CRT (-bm).
CF = -q -bt=nt -bm -s -ox -5r -fp5 -fpi87

RETROSQ.EXE: retrosq.obj jpeg.obj slimplay.obj retrosq.res
	wlink system nt_win option quiet name $@ file retrosq.obj,jpeg.obj,slimplay.obj library wsock32.lib,winmm.lib
	wrc -q -bt=nt retrosq.res $@

retrosq.obj: retrosq.c slimplay.h
	wcc386 $(CF) -fo=$@ retrosq.c
jpeg.obj: jpeg.c stb_image.h
	wcc386 $(CF) -fo=$@ jpeg.c
slimplay.obj: slimplay.c slimplay.h minimp3.h
	wcc386 $(CF) -za99 -otexan -fo=$@ slimplay.c
retrosq.res: retrosq.rc retrosq.ico
	wrc -q -bt=nt -r retrosq.rc -fo=$@

slimplay-test: slimplay.c slimplay.h minimp3.h
	gcc -DSLIMPLAY_TEST -O1 -w -o $@ slimplay.c -lpthread -lm

clean:
	rm -f *.obj *.o *.res RETROSQ.EXE slimplay-test
