import cv2
import numpy as np

points1 = []
points2 = []
isRunning = True

image1 = cv2.imread("./calibration/calibrationImage0.jpg")
image2 = cv2.imread("./calibration/calibrationImage1.jpg")

automaticCalibration = str(input("Souhaitez vous faire la calibration de manière automatique ? (y/n)"))

def computeFundamentalMatrix(points1, points2, computationMethod):
    F, mask = cv2.findFundamentalMat(points1, points2, computationMethod)
    with open("./calibration/results.txt", 'w') as file:
        file.truncate()
        F = F.flatten("C")
        for i in range(len(F)):
            coefficientString = str(F[i]) + "\n"
            file.write(coefficientString)
    return mask

if (automaticCalibration == "y"):
    # Instatiation de l'objet permettant de calculer la Scale Invariant Feature Transform.
    sift = cv2.SIFT_create()

    greyImage1 = cv2.cvtColor(image1, cv2.COLOR_RGB2GRAY)
    greyImage2 = cv2.cvtColor(image2, cv2.COLOR_RGB2GRAY)

    # Récupération des keyPoints et des descripteurs liés à ces points.
    keyPoints1, descriptors1 = sift.detectAndCompute(greyImage1, None)
    keyPoints2, descriptors2 = sift.detectAndCompute(greyImage2, None)

    image1 = cv2.drawKeypoints(image1, keyPoints1, image1)
    image2 = cv2.drawKeypoints(image2, keyPoints2, image2)

    # Correspondance des descripteurs de l'image 1 avec ceux de l'image 2.
    matcher = cv2.BFMatcher(cv2.NORM_L2, False)
    matchPairs = matcher.knnMatch(descriptors1, descriptors2, k=2)
    goodMatches = []

    # Test de Lowe : Vérification de la non-ambiguité d'une détection.
    for (m, n) in matchPairs:
        if m.distance < n.distance * 0.85:
            goodMatches.append(m)

    correctPoints1 = []
    correctPoints2 = []

    # Récupération des bons keypoints depuis la liste initiale.
    for point in goodMatches:
        correctPoints1.append(keyPoints1[point.queryIdx].pt)
        correctPoints2.append(keyPoints2[point.trainIdx].pt)

    correctPoints1 = np.array(correctPoints1, dtype=np.float32)
    correctPoints2 = np.array(correctPoints2, dtype=np.float32)

    mask = computeFundamentalMatrix(correctPoints1, correctPoints2, cv2.FM_RANSAC)

    realGoodMatches = []
    for i in range (len(goodMatches)):
        if (mask[i][0] == 1):
            realGoodMatches.append(goodMatches[i])

    finalImage = cv2.drawMatches(image1, keyPoints1, image2, keyPoints2, realGoodMatches, None, flags=cv2.DrawMatchesFlags_NOT_DRAW_SINGLE_POINTS)

    cv2.imshow("Images de calibration", finalImage)
    cv2.waitKey(10000)
    cv2.destroyAllWindows()
else:
    cv2.imshow("Image de calibration 1", image1)
    cv2.imshow("Image de calibration 2", image2)

    # Création d'une fonction appelée à chaque clic droit pour récupérer les coordonnées du point.
    def selectPoint(event, x, y, flags, param):
        if (event == cv2.EVENT_LBUTTONDOWN):
            if (param == 0):
                points1.append([x, y])
                cv2.circle(image1, (x, y), 2, (0, 0, 255), 2)
                cv2.imshow("Image de calibration 1", image1)
            elif (param == 1):
                points2.append([x, y])
                cv2.circle(image2, (x, y), 2, (0, 0, 255), 2)
                cv2.imshow("Image de calibration 2", image2)

    # Liaison du callback aux images.
    cv2.setMouseCallback("Image de calibration 1", selectPoint, 0)
    cv2.setMouseCallback("Image de calibration 2", selectPoint, 1)

    while (isRunning):
        # Lorsque le bon nombre de point à été sélectionné
        if (len(points1) == len(points2) and len(points1) == 8):
            isRunning = False
            points1 = np.array(points1, dtype=float)
            points2 = np.array(points2, dtype=float)
            computeFundamentalMatrix(points1, points2, cv2.FM_8POINT)
            cv2.destroyAllWindows()
        cv2.waitKey(500)