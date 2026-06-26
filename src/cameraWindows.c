#include "cameraWindows.h"

/* gcc -Wall -Wextra cameraWindows.c -o cameraWindows -I../include -L../lib -lmingw32 -lSDL2main -lSDL2 -lmfplat -lmf -lmfreadwrite -lmfuuid -lole32 && cameraWindows */

volatile sig_atomic_t isRunning = 1;

void sigIntHandler(int sig) {
    (void) sig;
    isRunning = 0;
}


int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("ERREUR : Veuillez spécifier une largeur, une hauteur d'image et un numéro de caméra.\n");
        exit(EXIT_FAILURE);
    }

    int width;
    if ((width = atoi(argv[1])) == 0) {
        printf("ERREUR : Impossible de récupérer la largeur voulue.\n");
        exit(EXIT_FAILURE);
    }

    int height;
    if ((height = atoi(argv[2])) == 0) {
        printf("ERREUR : Impossible de récupérer la hauteur voulue.\n");
        exit(EXIT_FAILURE);
    }

    int cameraIndex = atoi(argv[3]);

    /* Initialisation du handler d'interruption. */
    signal(SIGINT, sigIntHandler);

    /* Initialisation de la bibliothèque COM. */
    HRESULT comHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr)) {
        printf("ERREUR : L'initialisation de la bibliothèque COM à échoué.\n");
        CoUninitialize();
        exit(EXIT_FAILURE);
    }

    /* Initialisation de Media Foundation. */
    HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(mfHr)) {
        printf("ERREUR : L'initialisation de Media Foundation à échouée.\n");
        MFShutdown();
        CoUninitialize();
        exit(EXIT_FAILURE);
    }

    IMFActivate **ppDevices = NULL;
    IMFAttributes *pAttributes = NULL;
    UINT32 deviceCount = 0;

    HRESULT attributesHr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(attributesHr)) {
        printf("L'initialisation des attributs du magasin à échouée.\n");
        goto end_program;
    }

    pAttributes->lpVtbl->SetGUID(pAttributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    /* Récupération des devices disponibles. */
    HRESULT sourcesHr = MFEnumDeviceSources(pAttributes, &ppDevices, &deviceCount);
    if (FAILED(sourcesHr)) {
        printf("ERREUR : La détection des sources vidéo n'a pas pu se faire.\n");
        goto end_program;
    }

    /* Affichage des devices disponibles. */
    printf("Détection de %d capteurs disponibles pour la capture vidéo :\n", deviceCount);
    for (int i = 0; i < (int) deviceCount; i++) {
        WCHAR *deviceName;
        UINT32 nameLength = 0;
        ppDevices[i]->lpVtbl->GetAllocatedString(ppDevices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &deviceName, &nameLength);
        printf("\t - %ls\n", deviceName);
        CoTaskMemFree(deviceName);
    }

    if (cameraIndex >= (int) deviceCount) {
        printf("ERREUR : Veuillez saisir un index de caméra valide.\n");
        goto end_program;
    }

    pAttributes->lpVtbl->Release(pAttributes);

    /* Récupération / activation de la webcam. */
    IMFActivate *webcamActivate = ppDevices[cameraIndex];
    IMFMediaSource *webcamSource = NULL;
    HRESULT activateHr = webcamActivate->lpVtbl->ActivateObject(webcamActivate, &IID_IMFMediaSource, (void **) &webcamSource);
    if (FAILED(activateHr)) {
        printf("ERREUR : Impossible d'activer l'objet de la caméra.\n");
        goto end_program;
    }

    /* Création d'un source reader. */
    IMFSourceReader *pSourceReader = NULL;
    IMFAttributes *sourceReaderAttributes = NULL;
    attributesHr = MFCreateAttributes(&sourceReaderAttributes, 1);
    if (FAILED(attributesHr)) {
        printf("L'initialisation des attributs du magasin à échouée.\n");
        goto end_program;
    }

    sourceReaderAttributes->lpVtbl->SetUINT32(sourceReaderAttributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, (UINT32) 1);
    HRESULT sourceReaderHr = MFCreateSourceReaderFromMediaSource(webcamSource, sourceReaderAttributes, &pSourceReader);
    if (FAILED(sourceReaderHr)) {
        printf("ERREUR : Le source reader n'a pas pu être créé.\n");
        goto end_program;
    }

    /* Récupération des configurations disponibles. */
    IMFMediaType *desiredMediaType = NULL;
    IMFMediaType *mediaType = NULL;
    int i = 0;
    HRESULT hr = pSourceReader->lpVtbl->GetNativeMediaType(pSourceReader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &mediaType);
    printf("Voici les résolutions disponibles pour la caméra :\n");
    while (SUCCEEDED(hr)) {
        UINT64 resolutionSize;
        mediaType->lpVtbl->GetUINT64(mediaType, &MF_MT_FRAME_SIZE, &resolutionSize);
        printf("\t - %d:%d\n", (int) (resolutionSize >> 32), (int) (resolutionSize & 0xFFFFFFFF));
        i++;
        if (((int) (resolutionSize >> 32) == width) && ((int) (resolutionSize & 0xFFFFFFFF) == height)) {
            desiredMediaType = mediaType;
            break;
        } else {
            mediaType->lpVtbl->Release(mediaType);
        }
        hr = pSourceReader->lpVtbl->GetNativeMediaType(pSourceReader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &mediaType);
    }

    if (desiredMediaType == NULL) {
        printf("ERREUR : Aucune configuration ne correspond avec la résolution recherchée, veuillez sélectionner une résolution valide.\n");
        goto end_program;
    }

    /* Configuration des options de la caméra. */
    UINT64 frameSize = 0;
    UINT64 frameRate = 0;
    desiredMediaType->lpVtbl->GetUINT64(desiredMediaType, &MF_MT_FRAME_SIZE, &frameSize);
    desiredMediaType->lpVtbl->GetUINT64(desiredMediaType, &MF_MT_FRAME_RATE, &frameRate);

    IMFMediaType *pCleanType = NULL;
    MFCreateMediaType(&pCleanType);

    pCleanType->lpVtbl->SetGUID(pCleanType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    pCleanType->lpVtbl->SetGUID(pCleanType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    pCleanType->lpVtbl->SetUINT64(pCleanType, &MF_MT_FRAME_SIZE, frameSize);
    
    if (frameRate != 0) {
        pCleanType->lpVtbl->SetUINT64(pCleanType, &MF_MT_FRAME_RATE, frameRate);
    }

    /* Passage de la configuration en 2 étapes : d'abord en la forçant pour le matériel. */
    HRESULT setNativeHr = pSourceReader->lpVtbl->SetCurrentMediaType(pSourceReader, (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, desiredMediaType);
    if (FAILED(setNativeHr)) {
        printf("ERREUR : Impossible de forcer la résolution matérielle.\n");
        goto end_program;
    }

    /* Puis ensuite en la renseignant au logiciel. */
    HRESULT setMediaTypeHr = pSourceReader->lpVtbl->SetCurrentMediaType(pSourceReader, (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pCleanType);
    if (FAILED(setMediaTypeHr)) {
        printf("ERREUR : Le format RGB32 n'a pas pu être mis à la webcam.\n");
        goto end_program;
    }

    pCleanType->lpVtbl->Release(pCleanType);
    desiredMediaType->lpVtbl->Release(desiredMediaType);

    IMFMediaType *pActualType = NULL;
    pSourceReader->lpVtbl->GetCurrentMediaType(pSourceReader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pActualType);

    UINT64 actualSize;
    pActualType->lpVtbl->GetUINT64(pActualType, &MF_MT_FRAME_SIZE, &actualSize);

    int windowWidth = actualSize >> 32;
    int windowHeight = actualSize & 0xFFFFFFFF;
    printf("Taille réelle obtenue depuis la caméra : %d:%d", windowWidth, windowHeight);
    pActualType->lpVtbl->Release(pActualType);

    sourceReaderAttributes->lpVtbl->Release(sourceReaderAttributes);

    /* Initialisation de SDL. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("ERREUR : SDL n'a pas pu être lancé.\n");
        goto end_program;
    }

    /* Création de la fenêtre de visualisation. */
    SDL_Window *window = SDL_CreateWindow("Visualisation du flux vidéo de la caméra", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowWidth, windowHeight, SDL_WINDOW_RESIZABLE);

    if (window == NULL) {
        printf("ERREUR : La fenêtre SDL n'a pas pu être créée.\n");
        goto end_program;
    }

    /* Création de la texture et du renderer. */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_TARGETTEXTURE);

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, windowWidth, windowHeight);

    SDL_Event event;
    while(isRunning == 1) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                isRunning = 0;
            }
        }

        /* Récupération de l'image, mise à jour de la fenêtre. */
        DWORD actualStreamIndex;
        DWORD streamFlags;
        LONGLONG timeStamp;
        IMFSample *sample = NULL;
        pSourceReader->lpVtbl->ReadSample(pSourceReader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actualStreamIndex, &streamFlags, &timeStamp, &sample);
        if (sample != NULL) {
            IMFMediaBuffer *imageBuffer = NULL;
            sample->lpVtbl->ConvertToContiguousBuffer(sample, &imageBuffer);
            if (imageBuffer != NULL) {
                /* Il s'agit d'un buffer continu de pixels représentant l'image. */
                BYTE *pbBuffer = NULL;
                imageBuffer->lpVtbl->Lock(imageBuffer, &pbBuffer, NULL, NULL);
                SDL_UpdateTexture(texture, NULL, pbBuffer, windowWidth * 4);
                imageBuffer->lpVtbl->Unlock(imageBuffer);
                imageBuffer->lpVtbl->Release(imageBuffer);
                sample->lpVtbl->Release(sample);
            }
        }

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        SDL_Delay(1000 / TARGET_FPS);
    }

    /* Fermeture du programme. */
    end_program:
    MFShutdown();
    CoUninitialize();

    /* Nettoyage des devices récupérés. */
    for (int i = 0; i < (int) deviceCount; i++) {
        ppDevices[i]->lpVtbl->Release(ppDevices[i]);
    }
    CoTaskMemFree(ppDevices);
    return 0;
}