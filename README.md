# A C-written distributed platform for Jetson Orin Nanos.

It follows a simple Client --> Orchestrator --> Workers model, where multiple clients submit tasks in the form of a .cu file, and get the result back through the terminal.

## Each client submits a file that has to follow strict rules :
- The file has to be a .cu file to use fully the power of the worker cards,
- The file must compile, as the workers will do the compilation and execution jobs,
- The file must output the desired results in the terminal for the cards to send it back.
The resulting task also has a priority attached, given by the client depending on the priority of the task.

## Orchestrator functioning :
As of right now, the orchestrator do not split the tasks themselves, rather it distributes the load across all cards in order to garantee the quickest service possible.
The worker cards are detected automatically in the network by the reception of a monitoring message, which triggers the start of a thread (1 per card) to communicate with the said cards.
It uses multiple threads to catch incoming client requests (1 per client) and spawns them only when needed (when a client submits a task).
The orchestrator will always choose the card with the least full queue, to try to minimize the client's waiting time.

## Workers functioning :
The workers communicate with the orchestrator in two ways :
- They can inform it of their available queue size and their (de)connection with monitoring messages.
- And they can get/send tasks/results.

Each worker card has a queue of fixed size, and will process tasks in its queue in ascending order of priority. After a task arrive, the worker will proceed to sort the queue depending on the priority of the tasks.
Any new task submitted when the card's queues are full will be dropped and the client will be notified of the drop (or non-availability of the workers).
In order to reduce downtime, workers use two threads : one to received tasks, send the monitoring messages and sort the queue, and the other to execute the first task in the queue.

## Future axis of development :
- Accept a user-sent splitting function and aggregation function and re-wire the logic to split the tasks between the workers.
- Create a UI for the clients to ease the submitting process and for the orchestrator in order to visualize important metrics.
- Make use of TensorRT in order to run inference models from the users.
