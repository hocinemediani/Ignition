# An Edge-AI Video Streaming Platform for Jetson Orin Nanos

It follows a direct Client <--> Jetson Edge-AI Node model, where clients connect directly to a Jetson board to receive a real-time object detection stream.

## Architecture and Connections:
- **Autonomous Nodes:** Each Jetson board operates independently as an Edge-AI node, managing its own camera capture, model inference, and network distribution.
- **Multi-Client Support:** The network architecture allows up to 5 concurrent client connections simultaneously per Jetson node without degrading capture performance.
- **Direct Streaming:** Clients establish direct TCP connections to the nodes, removing any intermediary orchestrator overhead to guarantee minimal latency.

## Jetson Node Functioning:
The processing pipeline uses a hybrid C/Python architecture for maximum throughput:
- **Video Capture & Memory Management:** Video frames are captured directly from the hardware using the Linux V4L2 API in C. Shared memory mapping (`mmap`) is utilized to access the camera video buffers with zero-copy overhead.
- **Shared Library Interface:** Low-level networking, thread management, and V4L2 capture controls are compiled into a shared library (`libcamera.so`), which interacts directly with the Python runtime via `ctypes`.
- **Graph Optimization (GraphSurgeon):** Before inference, the baseline YOLOv8 ONNX model undergoes optimizations. Image normalization, channel transposition, and a dynamic Batched Non-Maximum Suppression (NMS) layer are injected directly into the model graph to offload post-processing (almost) entirely to the GPU.
- **TensorRT Inference:** Execution is driven by NVIDIA TensorRT and PyCUDA. The system binds CUDA managed memory buffers directly to the TensorRT execution context for asynchronous processing and uses the unified memory system Jetson Nanos have to avoid useless data copy. Processed frames are encoded to JPEG and dispatched over TCP sockets.

## Client Implementations:
- **Standard Client (`client.c`):** Connects to a single Jetson node, decodes the incoming JPEG-encoded stream using `stb_image`, and renders the video in real-time within an SDL2 window.
- **Stereo Client (`clientStereo.c`):** Connects to two Jetson nodes simultaneously to capture dual streams, aggregates the video data, and runs a comparison logic to detect and log common bounding boxes directly to the terminal.

Shield: [![CC BY-NC-ND 4.0][cc-by-nc-nd-shield]][cc-by-nc-nd]

This work is licensed under a
[Creative Commons Attribution-NonCommercial-NoDerivs 4.0 International License][cc-by-nc-nd].

[![CC BY-NC-ND 4.0][cc-by-nc-nd-image]][cc-by-nc-nd]

[cc-by-nc-nd]: http://creativecommons.org/licenses/by-nc-nd/4.0/
[cc-by-nc-nd-image]: https://licensebuttons.net/l/by-nc-nd/4.0/88x31.png
[cc-by-nc-nd-shield]: https://img.shields.io/badge/License-CC%20BY--NC--ND%204.0-lightgrey.svg
