CC = gcc

CFLAGS = -Wall

LIBS = -lncurses 
MATH_ONLY = -lm

all: main process_Drone BlackBoard process_In process_Ob process_Ta watchdog

system_logger.o: system_logger.c
	$(CC) $(CFLAGS) -c system_logger.c -o system_logger.o

main: main.c system_logger.o
	$(CC) $(CFLAGS) main.c system_logger.o -o main

process_Drone: process_Drone.c system_logger.o
	$(CC) $(CFLAGS) process_Drone.c system_logger.o -o process_Drone $(MATH_ONLY)

BlackBoard: BlackBoard.c system_logger.o
	$(CC) $(CFLAGS) BlackBoard.c system_logger.o -o BlackBoard $(LIBS) $(MATH_ONLY)

process_In: process_In.c system_logger.o
	$(CC) $(CFLAGS) process_In.c system_logger.o -o process_In

process_Ob: process_Ob.c system_logger.o
	$(CC) $(CFLAGS) process_Ob.c system_logger.o -o process_Ob

process_Ta: process_Ta.c system_logger.o
	$(CC) $(CFLAGS) process_Ta.c system_logger.o -o process_Ta

watchdog: watchdog.c system_logger.o
	$(CC) $(CFLAGS) watchdog.c system_logger.o -o watchdog

clean:
	rm main process_Drone BlackBoard process_In process_Ob process_Ta watchdog system_logger.o					
