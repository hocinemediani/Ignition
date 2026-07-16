import cv2
import numpy as np

points1 = []
points2 = []
isRunning = True

image1 = cv2.imread("./calibration/calibrationImage0.jpg")
image2 = cv2.imread("./calibration/calibrationImage1.jpg")

cv2.imshow("Image de calibration 1", image1)
cv2.imshow("Image de calibration 2", image2)

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

cv2.setMouseCallback("Image de calibration 1", selectPoint, 0)
cv2.setMouseCallback("Image de calibration 2", selectPoint, 1)

while (isRunning):
    if (len(points1) == len(points2) and len(points1) == 15):
        isRunning = False
        points1 = np.array(points1, dtype=float)
        points2 = np.array(points2, dtype=float)
        F, mask = cv2.findFundamentalMat(points1, points2, cv2.FM_RANSAC)
        with open("./calibration/results.txt", 'w') as file:
            file.truncate()
            F = F.flatten("C")
            for i in range(len(F)):
                coefficientString = str(F[i]) + "\n"
                file.write(coefficientString)
        cv2.destroyAllWindows()
    cv2.waitKey(500)