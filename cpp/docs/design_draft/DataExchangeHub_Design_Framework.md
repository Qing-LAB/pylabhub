# Data Exchange Hub Module: Design Framework

## 1. Goal
The goal is to provide two different mechanisms for sharing/communicating data among different programs/components that will be integrated through the `pylabhub-utils` library. This is what we hope the `pylabhub-hubshell` will eventually be used for: a hub that connects the physical world/data with data users, who need to store, read, and analyze data either in real-time or post-experiments. The two mechanisms would be:
1.  **Shared Memory Based:** Serving real-time, large block data communication for high performance, high capacity, and short response time.
2.  **ZeroMQ Based Messaging:** Mainly text-based messages, for low-cost, light command communication, state exchange, and key information communications.

## 2. Technical Requirements
### 2.1. Scenarios to Address
*   **Scenario 1: Real-time Experiment Integration and Control.** Running an experiment with various instruments that need integration, but lacking existing tools. Data acquisition requires real-time control for storage, parameter tuning, automation, data logging, etc. The system needs to combine tools to improve streaming datasets to storage, present data to the user, accept user input/direction, and execute tasks like changing experiment parameters. All acquisition states and operations must be faithfully recorded with timestamps to prevent data manipulation and loss of records.

## 3. Possible Tools
* [Your ideas here]
