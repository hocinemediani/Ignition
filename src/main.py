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

# Liste des classes identifiables.
COCO_CLASSES = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", 
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", 
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", 
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", 
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", 
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", 
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", 
    "chair", "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", 
    "mouse", "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", 
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
]

CLASS_COLOR = np.random.randint(0, 256, size=(80, 3), dtype=int)

# Configuration des différentes fonctions qui seront appelées.

## void sendImage(void *image, uint32_t imageSize);
lib.sendImage.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
lib.sendImage.restype = None

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
camWidth = int(input("Quelle est la largeur voulue pour le flux video ? "))
camHeight = int(input("Quelle est la hauteur voulue pour le flux video ? "))
camIndex = int(input("Quel est l'index de la caméra voulue ? "))
lib.initializeCamera(camWidth, camHeight, camIndex)

camInfo = lib.getCameraInfo()
cameraWidth = camInfo.width
cameraHeight = camInfo.height

time.sleep(0.5)

# Optimisation du modèle.
if (str(input("Souhaitez vous enregistrer les modifications au modèle ? ")) == "y"):
    onnxGraph = graphsurgeon.import_onnx(onnx.load("./models/yolov8n.onnx"))

    ## Normalisation de l'input.
    newInput = graphsurgeon.Variable("new-input", np.float32, (1, 640, 640, 3))
    twoFiftyFive = graphsurgeon.Constant("two-fifty-five", np.array([255.0], np.float32))
    normalizedInput = graphsurgeon.Variable("normalized-input", np.float32, (1, 640, 640, 3))
    divideInputNode = graphsurgeon.Node("Div", "divide-input-node", inputs=[newInput, twoFiftyFive], outputs=[normalizedInput])
    onnxGraph.nodes.append(divideInputNode)

    ## Transposition des canaux.
    transposeInputNode = graphsurgeon.Node("Transpose", "transpose-input", {"perm": [0, 3, 1, 2]}, [normalizedInput], [onnxGraph.inputs[0]])
    onnxGraph.nodes.append(transposeInputNode)
    onnxGraph.inputs = [newInput]

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
                                                                    "topK": 2048,
                                                                    "keepTopK": 20,
                                                                    "scoreThreshold": 0.5,
                                                                    "iouThreshold": 0.3,
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

## Allocation des buffers d'entrée et de sortie.
for i in range (0, numTensors):
    trtDType = trtEngine.get_tensor_dtype(trtEngine.get_tensor_name(i))
    correctType = np.float32
    if (trtDType == trt.DataType.INT32):
        correctType = np.int32

    ## Allocation des buffers partagés.
    buffers.append(cuda.managed_empty(
                                tuple(trtEngine.get_tensor_shape(trtEngine.get_tensor_name(i))),
                                correctType,
                                mem_flags=cuda.mem_attach_flags.GLOBAL))
    
    ## Liaison (enregistrement) de l'adresse du buffer device dans le contexte TensorRT.
    context.set_tensor_address(trtEngine.get_tensor_name(i), buffers[i].ctypes.data)

# Définition de la fenêtre de prévisualisation.
windowName = "Flux Jetson Orin Nano"
#cv2.namedWindow(windowName, cv2.WINDOW_NORMAL)

timer1 = 0
timer2 = 0
timer3 = 0
timer4 = 0
timer5 = 0
numFrames = 0

# Boucle de fonctionnement.
while (True):
    # Récupération de l'image.
    start = time.time()

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

    ## Copie du pointeur pour pouvoir libérer au plus vite le buffer et éviter les flux clunky.
    rawImage = np.ctypeslib.as_array(pixelsArray).copy()

    # Relachement du mutex associé au buffer.
    lib.releaseLastFrame(message.bufferIndex)

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

    ## Ajout de la taille du batch pour correspondre aux dimensions du tenseur d'entrée (1, 3, 640, 640).
    correctImage = np.expand_dims(fullRgbImage, axis=0)

    ## Aplatissement de l'image en un vecteur 1D.
    correctImage = np.ascontiguousarray(correctImage)
    image = np.ravel(correctImage)

    # Inférence.
    cudaStream = cuda.Stream()
    
    ## Copie de l'image dans le buffer d'entrée.
    np.copyto(buffers[0].ravel(), image)

    ## Lancement de l'inférence.
    context.execute_async_v3(cudaStream.handle)

    ## Attente de la copie du résultat.
    cudaStream.synchronize()

    # Dessin des boîtes de détection.

    ## Récupération des boîtes et des prédictions / scores de confiance.
    numBoxes = buffers[1].item()
    for i in range (0, numBoxes):
        classNumber = int(buffers[4][0][i])
        boxColor = tuple(CLASS_COLOR[classNumber].tolist())
        coordinates = [int(buffers[2][0][i][j]) for j in range(0, 4)]
        detectionString = COCO_CLASSES[classNumber] + " : " + str(round(buffers[3][0][i], 3)) + "%"
        cv2.rectangle(fullRgbImage, (coordinates[0], coordinates[1]), (coordinates[2], coordinates[3]), boxColor, 5)
        cv2.putText(fullRgbImage, detectionString, (coordinates[0], coordinates[1]), cv2.FONT_HERSHEY_SIMPLEX, 1, boxColor, 2)

    ## Affichage de l'image à l'écran / envoi de l'image.
    (unused, jpegArray) = cv2.imencode(".jpg", cv2.cvtColor(fullRgbImage, cv2.COLOR_RGB2BGR))
    imageToSend = jpegArray.ctypes.data_as(ctypes.c_void_p)
    imageSize = jpegArray.size
    lib.sendImage(imageToSend, imageSize)
    end = time.time()
    #cv2.imshow(windowName, cv2.cvtColor(fullRgbImage, cv2.COLOR_RGB2BGR))
    #cv2.setWindowTitle("Flux Jetson Orin Nano", "Flux Jetson Orin Nano, FPS : " + str(int(1 / (end - start))))

cv2.destroyAllWindows()
