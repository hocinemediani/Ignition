# An Orchestrated Inference Platform for Jetson Orin Nanos

It follows a Client <--> Orchestrator <--> Workers model, where multiple clients can submit custom machine learning models or inference tasks, and get the results back through the terminal.

## Architecture and Load Balancing :
- **Centralized Orchestration:** The orchestrator manages a cluster of Jetson Edge-AI nodes (workers). It dynamically tracks the state and available queue size of each connected worker via dedicated monitoring sockets (receiving `HELLO`, `INFO`, and `BYE` messages).
- **Smart Task Distribution:** When a client submits an inference request, the orchestrator explores the cluster state and routes the data to the worker with the least full queue. This ensures optimal load balancing and minimizes client waiting time.
- **Priority-Based Execution:** Each Jetson worker maintains a local queue. Upon receiving a new task, the worker sorts its queue based on the client-assigned priority level, ensuring that critical requests are processed first.

## Dual-Mode Client :
Clients connect to the orchestrator and can interact with the platform in two distinct ways:
1. **Model Submission (`client <model.onnx> <inference.cpp>`):** Clients can upload new ONNX models alongside their C++ inference logic. The orchestrator broadcasts these files to all connected Jetson nodes. Each worker then forks a process to locally compile the C++ file using `g++`, linking it against NVIDIA TensorRT and CUDA libraries. Once compiled, the model becomes globally available across the cluster.
2. **Inference Request (`client <dataFile> <modelName> <priority>`):** Clients submit a data file (e.g., an image) along with the name of the desired model and a priority level. The orchestrator delegates the file to the optimal worker. The worker executes the requested compiled model on the data, pipes the output to a temporary result file, and sends the analytical results back through the orchestrator to the client's terminal.

## Worker Node Functioning :
To prevent blocking the network layer during heavy operations, workers utilize a multi-threaded architecture with condition variables (`pthread_cond_t`):
- A **Monitoring/Networking Thread** continuously listens for incoming orchestrator requests, writes incoming files to the disk, updates the priority queue, and sends queue-size updates back to the orchestrator.
- An **Execution Loop** waits for tasks to appear in the queue. It forks a new process to either compile new models or execute inference binaries, captures the `stdout` of the execution, and dispatches the results back to the client.

Shield: [![CC BY-NC-ND 4.0][cc-by-nc-nd-shield]][cc-by-nc-nd]

This work is licensed under a
[Creative Commons Attribution-NonCommercial-NoDerivs 4.0 International License][cc-by-nc-nd].

[![CC BY-NC-ND 4.0][cc-by-nc-nd-image]][cc-by-nc-nd]

[cc-by-nc-nd]: http://creativecommons.org/licenses/by-nc-nd/4.0/
[cc-by-nc-nd-image]: https://licensebuttons.net/l/by-nc-nd/4.0/88x31.png
[cc-by-nc-nd-shield]: https://img.shields.io/badge/License-CC%20BY--NC--ND%204.0-lightgrey.svg
