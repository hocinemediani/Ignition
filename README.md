# A Distributed CUDA Processing Platform for Jetson Orin Nanos

It follows a Client <--> Orchestrator <--> Workers model, where multiple clients submit tasks in the form of a `.cu` (CUDA) file, and get the execution results back directly through their terminal.

## Client Submission Rules :
Each client submits a file that has to follow strict rules to be processed correctly by the cluster:
- **Extension:** The file *must* be a `.cu` file to fully utilize the GPU capabilities of the Jetson worker cards.
- **Compilation:** The source code must be valid and self-contained, as the workers will dynamically invoke `nvcc` to compile it on the fly.
- **Standard Output:** The file must output its desired results to the standard output (e.g., using `printf`). The worker nodes intercept `stdout` during execution to capture the data and send it back.
- **Priority Level:** The client must attach a priority integer to the task, which dictates its sorting order in the assigned worker's execution queue.

## Orchestrator Functioning :
The orchestrator acts as a centralized load balancer, distributing tasks without altering them:
- **Dynamic Discovery:** Worker cards are detected automatically on the network when they connect and send a `HELLO` monitoring message via a dedicated monitoring port.
- **Load Balancing:** When a client submits a `.cu` task, the orchestrator explores the cluster's state and routes the file to the worker with the most available space in its queue, minimizing the client's waiting time.
- **Task Routing:** It maintains a routing table mapped to unique Task IDs. This ensures that when a worker finishes computing a task, the orchestrator accurately forwards the text result back to the original client socket.

## Worker Node Functioning :
The workers run on the Jetson boards and communicate with the orchestrator using a dual-socket, dual-thread architecture to prevent network bottlenecks:
- **Monitoring & Queuing:** A dedicated networking thread continuously listens for incoming orchestrator requests. It receives `.cu` files, places them in a local queue, and immediately sorts them based on the client-assigned priority. It also sends continuous `INFO` updates to the orchestrator regarding its remaining queue capacity.
- **Compilation & Execution:** The main thread processes the queue continuously. For each task, it forks a child process to compile the code using `nvcc`. Upon a successful build, it forks again to execute the resulting binary, using `dup2` to pipe the standard output directly into a temporary `.txt` file. This result file is then read, shipped back to the orchestrator, and all temporary files are cleanly removed from the local disk.

Shield: [![CC BY-NC-ND 4.0][cc-by-nc-nd-shield]][cc-by-nc-nd]

This work is licensed under a
[Creative Commons Attribution-NonCommercial-NoDerivs 4.0 International License][cc-by-nc-nd].

[![CC BY-NC-ND 4.0][cc-by-nc-nd-image]][cc-by-nc-nd]

[cc-by-nc-nd]: http://creativecommons.org/licenses/by-nc-nd/4.0/
[cc-by-nc-nd-image]: https://licensebuttons.net/l/by-nc-nd/4.0/88x31.png
[cc-by-nc-nd-shield]: https://img.shields.io/badge/License-CC%20BY--NC--ND%204.0-lightgrey.svg
