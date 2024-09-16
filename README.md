# -nRF5340-TrafficSignals

# Traffic Light Control System

This project implements a traffic light control system in the Zephyr RTOS environment. The program receives sequences via the serial port that define the order, duration, and repetition of LED lights. It processes these sequences and controls the LEDs accordingly.

## Features

- **Receiving sequences from the serial port**: The program reads strings from the serial port containing color codes ('R' - red, 'G' - green, 'Y' - yellow), durations in milliseconds, and repetition counts.
- **Sequence processing**: The `dispatcher_task` parses the received sequences and places the commands into separate FIFO queues based on colors. Additionally, commands are added to a global command queue to ensure the lights turn on in the correct order.
- **Light control**: Each color has its own light task, which waits for its turn and turns on the LED for the specified duration. Light tasks use condition variables and mutexes for synchronization.
- **Repetition feature**: Sequences can be repeated multiple times using the 'T' character followed by the repetition count at the end of the sequence.

## Usage Instructions

1. **Compile and program the device**: Ensure that the Zephyr environment is correctly installed and the device is connected.
2. **Start the program**: The program initializes UART and GPIO devices and creates the necessary tasks.
3. **Input sequence**: Send a sequence via the serial port in the following format:


## Example:

This will turn on the red light for 1000 ms, the green light for 500 ms, and the yellow light for 1000 ms, repeating the sequence two times.
<video width="320" height="240" controls>
  <source src="example.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

## Structure Description

- **uart_receive_task**: Reads incoming messages from the serial port and stores them in a ring buffer.
- **dispatcher_task**: Processes the received messages, parses the sequences, and places the commands into FIFO queues and the command queue.
- **Light tasks (red_light_task, green_light_task, yellow_light_task)**: Wait for their turn in the command queue and turn on the LED for the specified duration.
- **Synchronization**: Mutexes and condition variables ensure that only one light is on at a time and that the lights turn on in the correct order.

