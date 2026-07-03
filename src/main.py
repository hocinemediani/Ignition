import onnx
import tensorrt as trt
import pycuda.driver as cuda
import pycuda.autoinit
import onnx_graphsurgeon as graphsurgeon
import ctypes
import cv2
import time
import numpy as np

# Importation de la librairie partagée compilée depuis le code C.
lib = ctypes.CDLL("./libcamera.so")

# Création de la structure que l'on recevra en retour de l'appel à getLastFrame.
class PythonMessage(ctypes.Structure):
    _fields_ = [
        ("start", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
        ("bufferIndex", ctypes.c_int)
    ]

class CameraInfo(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int)
    ]

# Configuration des différentes fonctions qui seront appelées.

## void loadEngine(char *modelPath);
lib.loadEngine.argtypes = [ctypes.c_char_p]
lib.loadEngine.restype = None

## struct pythonMessage getLastFrame();
lib.getLastFrame.restype = PythonMessage

## void releaseLastFrame(int index);
lib.releaseLastFrame.argtypes = [ctypes.c_int]
lib.releaseLastFrame.restype = None

## void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex);
lib.initializeCamera.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.initializeCamera.restype = None

## struct cameraInfo getCameraInfo();
lib.getCameraInfo.restype = CameraInfo

# Initialisation de l'environnement.
lib.initializeCamera(0, 0, 0)

camInfo = lib.getCameraInfo()
cameraWidth = camInfo.width
cameraHeight = camInfo.height

time.sleep(0.5)

# Optimisation du modèle.
onnxGraph = graphsurgeon.import_onnx(onnx.load("./models/yolov8n.onnx"))

## Transposition des boîtes et des classes.
transposeOutput = graphsurgeon.Variable("transpose-output", np.float32, (1, 8400, 84))
transposeNode = graphsurgeon.Node("Transpose", "transpose-node", {"perm": [0, 2, 1]}, [onnxGraph.outputs[0]], [transposeOutput])
onnxGraph.nodes.append(transposeNode)

## Séparation en 2 tenseurs : boîtes et classes.
boxesOutput = graphsurgeon.Variable("boxes-output", np.float32, (1, 8400, 4))
classesOutput = graphsurgeon.Variable("classes-output", np.float32, (1, 8400, 80))

boxesStart = graphsurgeon.Constant("boxes-start", np.array([0], dtype = np.int64))
classesStart = graphsurgeon.Constant("classes-start", np.array([4], dtype = np.int64))

boxesEnd = graphsurgeon.Constant("boxes-end", np.array([4], dtype = np.int64))
classesEnd = graphsurgeon.Constant("classes-end", np.array([84], dtype = np.int64))

axis = graphsurgeon.Constant("axis", np.array([2], dtype = np.int64))
step = graphsurgeon.Constant("step", np.array([1], dtype = np.int64))

boxesSliceNode = graphsurgeon.Node("Slice", "boxes-slice-node", inputs=[transposeOutput, boxesStart, boxesEnd, axis, step], outputs=[boxesOutput])
classesSliceNode = graphsurgeon.Node("Slice", "classes-slice-node", inputs=[transposeOutput, classesStart, classesEnd, axis, step], outputs=[classesOutput])
onnxGraph.nodes.append(boxesSliceNode)
onnxGraph.nodes.append(classesSliceNode)

## Ajout de la couche de Non Maximum Suppression.

### Conversion de x_centre, y_centre, largeur, hauteur à
###               x_min, y_min, x_max, y_max
centerX = graphsurgeon.Variable("center-x", np.float32, (1, 8400, 1))
centerY = graphsurgeon.Variable("center-y", np.float32, (1, 8400, 1))
width = graphsurgeon.Variable("width", np.float32, (1, 8400, 1))
height = graphsurgeon.Variable("height", np.float32, (1, 8400, 1))

xMin = graphsurgeon.Variable("x-min", np.float32, (1, 8400, 1))
yMin = graphsurgeon.Variable("y-min", np.float32, (1, 8400, 1))
xMax = graphsurgeon.Variable("x-max", np.float32, (1, 8400, 1))
yMax = graphsurgeon.Variable("y-max", np.float32, (1, 8400, 1))

coordinateSplitNode = graphsurgeon.Node("Split", "coordinate-slice-node", {"axis": 2}, [boxesOutput], [centerX, centerY, width, height])
onnxGraph.nodes.append(coordinateSplitNode)

### x_min = x_centre - (width / 2)
### y_min = y_centre - (height / 2)
### x_max = x_centre + (width / 2)
### y_max = y_centre + (height / 2)
two = graphsurgeon.Constant("two", np.array([2], np.float32))
halfHeight = graphsurgeon.Variable("half-height", np.float32, (1, 8400, 1))
halfWidth = graphsurgeon.Variable("half-width", np.float32, (1, 8400, 1))

halfHeightNode = graphsurgeon.Node("Div", "half-height-node", inputs=[height, two], outputs=[halfHeight])
halfWidthNode = graphsurgeon.Node("Div", "half-width-node", inputs=[width, two], outputs=[halfWidth])
onnxGraph.nodes.append(halfHeightNode)
onnxGraph.nodes.append(halfWidthNode)

xMinNode = graphsurgeon.Node("Sub", "x-min-node", inputs=[centerX, halfWidth], outputs=[xMin])
yMinNode = graphsurgeon.Node("Sub", "y-min-node", inputs=[centerY, halfHeight], outputs=[yMin])
xMaxNode = graphsurgeon.Node("Add", "x-max-node", inputs=[centerX, halfWidth], outputs=[xMax])
yMaxNode = graphsurgeon.Node("Add", "y-max-node", inputs=[centerY, halfHeight], outputs=[yMax])
onnxGraph.nodes.append(xMinNode)
onnxGraph.nodes.append(yMinNode)
onnxGraph.nodes.append(xMaxNode)
onnxGraph.nodes.append(yMaxNode)

### Concatenation des nouvelles variables en un seul tenseur.
correctBoxesOutput = graphsurgeon.Variable("correct-boxes-output", np.float32, (1, 8400, 4))
concatenateNode = graphsurgeon.Node("Concat", "concatenate-node", {"axis": 2}, [xMin, yMin, xMax, yMax], [correctBoxesOutput])
onnxGraph.nodes.append(concatenateNode)

nmsInput = graphsurgeon.Variable("nms-input", np.float32, (1, 8400, 1, 4))
unsqueezeAxis = graphsurgeon.Constant("unsqueeze-axis", np.array([2], np.int64))
unsqueezeNode = graphsurgeon.Node("Unsqueeze", "unsqueeze-node", inputs=[correctBoxesOutput, unsqueezeAxis], outputs=[nmsInput])
onnxGraph.nodes.append(unsqueezeNode)

### Résultats finaux du NSM.
numBoxes = graphsurgeon.Variable("num-boxes", np.int32, (1, 1))
filteredBoxes = graphsurgeon.Variable("filtered-boxes", np.float32, (1, 25, 4))
boxesScores = graphsurgeon.Variable("boxes-scores", np.float32, (1, 25))
boxesClasses = graphsurgeon.Variable("boxes-classes", np.float32, (1, 25))

nmsNode = graphsurgeon.Node("BatchedNMSDynamic_TRT", "nms-node", {"shareLocation": 1,
                                                                  "backgroundLabelId": -1,
                                                                  "numClasses": 80,
                                                                  "topK": 4096,
                                                                  "keepTopK": 25,
                                                                  "scoreThreshold": 0.5,
                                                                  "iouThreshold": 0.45,
                                                                  "clipBoxes": 0,
                                                                  "isNormalized": 0}, [nmsInput, classesOutput], [numBoxes, filteredBoxes, boxesScores, boxesClasses])
onnxGraph.nodes.append(nmsNode)

### Configuration de la réelle sortie du modèle.
onnxGraph.outputs = [numBoxes, filteredBoxes, boxesScores, boxesClasses]
onnxGraph.cleanup()

## Exportation du nouveau modèle ONNX.
newModel = graphsurgeon.export_onnx(onnxGraph)
onnx.save_model(newModel, "./models/yolov8nNMS.onnx")

# Vérification de l'existence (ou build) de l'engine.
lib.loadEngine(b"./models/yolov8nNMS.onnx")

# Load de l'engine et du contexte.
logger = trt.Logger(trt.Logger.ERROR)
trt.init_libnvinfer_plugins(logger, "")
runtime = trt.Runtime(logger)
with open("./models/yolov8n.engine", "rb") as engine:
    trtEngine = runtime.deserialize_cuda_engine(engine.read())
context = trtEngine.create_execution_context()

numTensors = trtEngine.num_io_tensors
buffers = []
memoryPointers = []

## Allocation des buffers d'entrée et de sortie.
for i in range (0, numTensors):
    trtDType = trtEngine.get_tensor_dtype(trtEngine.get_tensor_name(i))
    correctType = np.float32
    if (trtDType == trt.DataType.INT32):
        correctType = np.int32

    ## Allocation du buffer host (en RAM).
    buffers.append(cuda.pagelocked_empty(tuple(trtEngine.get_tensor_shape(trtEngine.get_tensor_name(i))), correctType))

    ## Allocation du buffer device (en VRAM).
    memoryPointers.append(cuda.mem_alloc(buffers[i].nbytes))
    
    ## Liaison (enregistrement) de l'adresse du buffer device dans le contexte TensorRT.
    context.set_tensor_address(trtEngine.get_tensor_name(i), int(memoryPointers[i]))

# Définition de la fenêtre de prévisualisation.
windowName = "Flux Jetson Orin Nano"
cv2.namedWindow(windowName, cv2.WINDOW_NORMAL)

# Boucle de fonctionnement.
while (True):
    # Récupération de l'image.

    ## Récupération de la dernière image obtenue.
    message = lib.getLastFrame()

    ## Sécurité permettant de fermer le programme en appuyant sur q.
    if cv2.waitKey(1) & 0xFF == ord("q"):
        break

    ## Vérification de l'information récupérée.
    if message.start is None or message.size <= 0:
        print("En attente de la prochaine frame...")
        continue
    
    ## Récupération du tableaux de pixels.
    ArrayType = ctypes.c_uint8 * message.size
    pixelsArray = ctypes.cast(message.start, ctypes.POINTER(ArrayType)).contents
    rawImage = np.ctypeslib.as_array(pixelsArray)

    # Préparation de l'image.

    ## Récupération des caractéristiques de l'image.
    scale = min((640 / cameraWidth), (640 / cameraHeight))
    imageWidth = int(cameraWidth * scale)
    imageHeight = int(cameraHeight * scale)
    topPadding = (640 - imageHeight) // 2
    bottomPadding = (640 - imageHeight) // 2 + (640 - imageHeight) % 2
    leftPadding = (640 - imageWidth) // 2
    rightPadding = (640 - imageWidth) // 2 + (640 - imageWidth) % 2

    ## Transformation du tableau contigu de pixels en une image.
    yuyvImage = rawImage.reshape((cameraHeight, cameraWidth, 2))

    ## Conversion de l'image du format YUYV (natif) à RGB.
    rgbImage = cv2.cvtColor(yuyvImage, cv2.COLOR_YUV2RGB_YUYV)

    ## Redimensionnement de l'image.
    resizedImage = cv2.resize(rgbImage, (int(cameraWidth * scale), int(cameraHeight * scale)), cv2.INTER_LINEAR)

    ## Ajout des black-boxes pour convenir aux dimensions du tenseur d'entrée.
    fullRgbImage = cv2.copyMakeBorder(resizedImage, topPadding, bottomPadding, leftPadding, rightPadding, cv2.BORDER_CONSTANT, (0, 0, 0))

    ## Normalisation de la valeur des pixels.
    normalizedImage = (fullRgbImage / 255.0).astype(np.float32)

    ## Passage de HWC à CHW.
    chwImage = np.transpose(normalizedImage, (2, 0, 1))

    ## Ajout de la taille du batch pour correspondre aux dimensions du tenseur d'entrée (1, 3, 640, 640).
    correctImage = np.expand_dims(chwImage, axis=0)

    ## Aplatissement de l'image en un vecteur 1D.
    correctImage = np.ascontiguousarray(correctImage)
    image = np.ravel(correctImage)

    # Inférence.
    cudaStream = cuda.Stream()
    
    ## Copie de l'image dans le buffer d'entrée.
    np.copyto(buffers[0].ravel(), image)
    cuda.memcpy_htod_async(memoryPointers[0], buffers[0], cudaStream)

    ## Lancement de l'inférence.
    context.execute_async_v3(cudaStream.handle)

    ## Récupération des résultats.
    for i in range (1, numTensors):
        cuda.memcpy_dtoh_async(buffers[i], memoryPointers[i], cudaStream)

    ## Attente de la copie du résultat.
    cudaStream.synchronize()

    ## Dessin des boîtes de détection.
    numBoxes = buffers[1].item()
    for i in range (0, numBoxes):
        coordinates = [int(buffers[2][0][i][j]) for j in range(0, 4)]
        cv2.rectangle(fullRgbImage, (coordinates[0], coordinates[1]), (coordinates[2], coordinates[3]), (255, 0, 0), 5)

    ## Affichage de l'image à l'écran.
    cv2.imshow(windowName, cv2.cvtColor(fullRgbImage, cv2.COLOR_RGB2BGR))

    # Relachement du mutex associé au buffer.
    lib.releaseLastFrame(message.bufferIndex)

cv2.destroyAllWindows()
