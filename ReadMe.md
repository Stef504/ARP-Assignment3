## Architecture Sketch
[📄 Click here to view the sketch of the architecture (PDF)](./Architecture.pdf)

## Active Components

### 🖥️ Master Process (`main.c`)

This is the master process tasked with generating the concurrent program. It employs the primitive fork() and execlp() functions to initiate each process. Additional primitives employed include pipes(), which assist in the communication between processes. Pipe() enables the reading and writing of data to designated pipes. The corresponding pipes (opening or closing) are indicated at each fork and all pipes are closed at the end of the code to ensure the program does not freeze / enter deadlocking. The wait() primitive is employed at the end of the code to guarantee that all parent processes await the completion of their child processes before returning. 

The kill(*process_name*, SIGTERM) signal sends signals to all other processes to terminate. This is executed when the drone process exits. If this signal did not work, the kill(*process_name*, SIGKILL) signal will then forcefully kill all other processes. This primitive is important in shutting down the entire code smoothly.

The respective **POSIX Macros** used in Main are: 
    - `WIFEXITED` - used to identify how each child process was terminated
    - `WEXITSTATUS` - used to extract the type of exit code 
    - `WIFSIGNALED` - if the child process was terminated by a signal

Main is responsible for executing the blackboard, drone physics, input handling, and obstacle and target generation.

The use of unnamed pipes() allows for fast and simple communication between related processes. While the use of the named pipes (FIFOs) allows for communication between unrelated processes.

### 📊 Server / Blackboard (`BlackBoard.c`)
This outlines the procedure for interprocess communication among all processes. All data on targets, obstacles, drone physics, and inputs is transmitted to the blackboard to facilitate the broadcasting of specific data to the corresponding processes. The process is executed via the initialisation of pipes provided through command line parameters (transmitted from Main) and the basic select() function.

The basic select() function enables the prevention of race conditions. 

- The data accepted from the **fdToBB** pipe is as listed: 
    -The drones current coordinates

- The data sent from the **fdFromBB** pipe is as listed: 
    - *refer to the Drone Process*

- The data accepted from **fdIn_BB** named pipe is as listed:
    - Commands such as 'a', 'q', and 'u'. *see operation instructions* 

- The data accepted from the **fdOb, fdTa** pipes are as listed:
    - Coordinates of obstacles and targets

Key algorithm of the blackboard:
- The blackboard additionally retrieves information from the parameter file.
- It initialises the requisite pipes, as specified by the command line arguments provided by the main function
- The window adjusts based on the user's preferences; however, each adjustment causes a reset of the drone's position.
- The drone's physics module transmits its location to the blackboard. The blackboard will display the drone's location on the window.
- The window is redrawn every iteration.
- The blackboard will notify the drone of its proximity to obstacles, its initial location at the game's starting point, and when the drone is restored to the home position.
- The obstacles and target generation transmit their coordinates to the blackboard to render on the window and establish if the drone is near the obstacle.
- Obstacles and targets are generated dynamically following a cumulative total of 20 generations.
- Obstacles, targets and the drone is clamped to the window size to adhere to the game boundaries
- The system utilises standard exit codes, and upon completion of the code, it closes all associated pipes.


### 🕹️ Drone Process (`process_Drone.c`)
- The drone's physics (motion and repulsive forces) are computed. 
- It initialises the requisite pipes, as specified by the command line arguments provided by the main function, and utilises the primitive select() for reading from the blackboard and input processes.
- The drone will move continously in one direction until either of the conditions are met:
    1. Brakes are applied
    2. Rest is applied
    3. An opposing direction was entered
- The speed of the drone increases in increments of 20%. A maximum of two boosts are applied. This relates to three consecutive clicks of the same letter. The drone can only slow down in the current direction if its opposite button is pressed 3 times. 

- The data accepted from the BlackBoard (**fdFromBB** pipe) is as listed: 
    - The intial position of the drone when the game is launch
    - Clamping of the drone when it reaches the boarder of the window to prevent vanishing
    - When the user resizes the window the drone is reset to the center 
    - When the user resets the drone

- The data sent to the BlackBoard (**fdToBB** pipe) is as listed:
    - Its current coordinates after the respected forces have been calculated

- The date accepted from the BlackBoard via the **fdRepul** pipe is as listed:
    - Data is only sent when the distance between the obstacle and the drone is below rho. The distance calculation is done in the Blackboard.
    - This function enables the calculation and addition of the repulsive forces. 
    - When the drone is notified about the data received, it turns off any boosts and removes the active key.
    - To visualise the repulsive force, a scaling factor has been applied and a maximum force has been implemented to reduce high velocity when being repelled. 

- The data received from the input process via the **fdIn** pipe is as listed:
    - The coordinates to control the drone

- Safety Factors:
    - The signal (SIGPIPE, SIG_IGN) mechanism to guarantee that the program does not terminate immediately if the input pipe is disrupted or closed
    - The data written to the BlackBoard and if it failed to write
    - The distance the repulsion force reacted, and the amount of force applied
    - The system utilises standard exit codes, and upon completion of the code, it closes all associated pipes.


### Input Process (`process_In.c`)
- This records the users input. Only the designated input commands will cause a reaction to both the drone and the blackboard.
- The process sends the user's input directly to the drone and the blackboard over distinct channels (**fdIn** and **fdIn_BB**). - The modification of the canonical mode removed the necessity for the user to press ENTER after each input.

Safety Factor:
- Signal (SIGPIPE, SIG_IGN) mechanism to guarantee that the program does not terminate immediately if the input pipe is disrupted or closed. Instead, it disregards the SIGPIPE and allows the client to manage the error to ensure the resetting of the canonical mode. 
- The system utilises standard exit codes, and upon completion of the code, it closes all associated pipes.


### 🚧 Obstacle and 🎯 Target Generator(`process_Ob.c`, `process_Ta.c`)
- The process reads the parameter file, initialises pipes provided through command-line options (transmitted from Main), and publishes coordinates. 
- The generation is clamped to the current window size but is adjusted accordingly in the BlackBoard.
- Target coordinates are generated and published every 7 seconds on the blackboard. 
- While obstacle coordinates are generated and publsihed every 5 seconds to the blackboard. 
- The system utilises standard exit codes, and upon completion of the code, it closes all associated pipes.

The different timings ensure that generations do not appear at once. The random generation relies on the time and PID, ensuring a unique sequence for generation. 
The readjustment of obstacles and targets as the window changes allows for an even spread of obstacles and targets.

### 🧾 Logging System (`system_logger.c`, `logger_custom.h`)

This module provides a **system-wide, multi-process safe logging utility** used by the Master, Blackboard, Drone, Input, and generator processes to write into the same file (`system.log`) without corruption problems.

`logger_custom.h` serves as an interface, which defines log levels such as **LOG_INFO**, **LOG_WARNING** and **LOG_CRITICAL** and provides easy-to-use macros that automatically report the file name, line number and specific function where a certain log occurred. This immediate location provides an easy way to consistently debug our program.

`system_logger.c` serves as the actual implementation of the logger's header file. In its principal function, **(void)logger_log**, it keeps track of everything that happens in the whole system and reports it in `system.log`. When a log is written, this program:

- Opens the log file.
- Gains exclusive lock of the file by using the **flock(..., LOCK_EX)** method, to prevent more than one log written in the same moment. 
- Writes the actual log information, composed by:
	- Timestamp
	- Process PID
	- Process name 
	- Log message
- Removes the exclusive lock
- Closes the log file

### 🧾 Process Registry Logger (`logger.h`)

This header file has the purpose of process discovery. It writes in a specific file (`process_log.log`) all the processes currently exist when launching the master process, and does this with two main methods:

- **log_process()**: when a process starts, its name and PID are written in `process_log.log`
- **get_pid_by_name()**: helper function, helps finding a PID if only a process name is known

This logger is separated from the other because it works as a way to keep track of all the processes, rather than a debugging tool. It is mainly checked by the watchdog to let it know of what processes it must check.


### 🐶 Watchdog (`watchdog.c`)

This process implements a **health monitoring supervisor** for the concurrent system. It periodically checks that registered processes respond to a “ping”, and terminates unresponsive ones.

Data sources and sinks:
- Input: `process_log.log`
	- The watchdog reads process names and PIDs written by `log_process()`.
	- It ignores its own PID and the `Master` entry.
- Output: `watchdog_log.log`
	- Operational log of watchdog actions.
	- Each line is written using file locking to avoid concurrency issues.

Key algorithm of the watchdog:
- Waits until `process_log.log` contains at least one process to monitor.
- Every `CHECK_INTERVAL` seconds, it reloads the process list (supports dynamic process creation).
- For each active process:
	1. Verifies the PID still exists (`kill(pid, 0)` as an existence probe).
	2. Sends a PING (`SIGUSR1`).
	3. Starts a timeout (`alarm(RESPONSE_TIMEOUT)`).
	4. Waits until either:
		- A PONG arrives (`SIGUSR2` handled by `response_handler`), or
		- Timeout fires (`SIGALRM` handled by `timeout_handler`).
	5. If timeout occurs, the watchdog force-kills the process (`SIGKILL`) and marks it inactive.

## Log files added

- `coordinates.log`: reports the current position of the drone and if the pipe writing has been successful or not.
- `process_log.log`: writes in it all the currently active processes with respective names and PIDs.
- `system.log`: global log file that reports process starting, updates and user inputs.
- `watchdog_log.log`: reports the output of the health cycle, i.e. checking if every currently active process works every time the security check is completed

## 3. List of Files
/ARP-Assignment2
|-main.c
|-BlackBoard.c
|-process_Drone.c
|-process_In.c
|-process_Ta.c
|-process_Ob.c
|-Parameter_File.txt
|-ReadMe.md
|-Makefile
|-Sketch_of_Architecture.pdf
|-system_logger.o
|-watchdog.c
|-logger.h
|-logger_custom.h

## 🛠️ Installation and Running

**1. Prerequisites**
Ensure all source files (`.c`) and the `Makefile` are in the same directory.

**2. Compilation**
Open your terminal in the project folder and run the build script:
```bash
make
./main
```
To clear executables, open your terminal in the project folder and run the following script:
```bash
make clean
```

## Viewing .log files

To view the `.log` files in the terminal the following commands should be run:

1. To view each `.log` file created
Open a new terminal in the project folder and run the following script:
```bash
ls *.log
```
2. A continuous window that monitors the operation of the `.log` files in real time:
Open a new terminal in the project folder and run the following script while the game is running:
```bash
tail -f watchdog_log.log process_log.log system.log 
```
3. View end-of-file content
Open a new terminal in the project folder and run the following script after the game is terminated:
```bash
tail watchdog_log.log process_log.log system.log
```
4. View the entire file history
Open a new terminal in the project folder and run the following script:
```bash
cat watchdog_log.log process_log.log system.log
```
5. View the entire file history in an easier to read format
It will be easier to read the file history if each `.log` is opened separately using the 
```bash 
cat 
```
To view separately the following commands should be executed:
Open a new terminal in the project folder and run the following scripts in different terminals. Every `cat` should be in a new terminal:
```bash
cat watchdog_log.log 
cat process_log.log 
cat system.log
cat coordinates_log.log
```

## 🕹️Operational Instructions
'e' - moves up
'c' - moves down
'f' - moves right
's' - moves left
'r' - moves north-eat
'x' - moves south-west
'w' - moves west-north
'v' - moves east-south

'd' - breaks
'a' - reset
'q' - quits the game
'p' - pauses the game
'u' - unpauses the game


## Fixes applied

- Drone, obstacles, targets and the window border are now colored and correctly visualized.
- Removed "square" writing artifact near the drone
- Fixed repulsion forces so that they make the drone "slide" around the obstacle rather than pushing it away

> **⚠️ Disclaimer:**
> Please wait for the drone to stop moving after the repulsion force was detected so as to provide the program enough time to react before pressing another key.

### Necessary Notes:
>All commits for the first assignment are accessible for evaluation on GitHub at https://github.com/Stef504/ARP.git.
>All commits for the second assignment are accessible for evaluation on GitHub at https://github.com/Sisoooo/ARP-Assignment2.git

First Assignment Comments:
These commits demonstrate our continued progress on the assignment. This project is a work in progress, primarily aimed towards ensuring that the interprocess communication via pipes is efficient and synchronised. Our second objective was to verify the accuracy of the computational logic and mathematics defining the obstacle and target generation, as well as the drone physics. The game's graphics will be enhanced during the project's final phases. We sincerely value your feedback to further our coding skills. 
