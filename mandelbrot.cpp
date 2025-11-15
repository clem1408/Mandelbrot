#include <opencv2/opencv.hpp>
#include <complex>
#include <iostream>
#include <omp.h>

using namespace std;
using namespace cv;

// ----------------------------------------------------------
// COMPUTE ESCAPE ITERATIONS FOR EACH PIXEL
// ----------------------------------------------------------
void computeEscapeValues(
    vector<vector<int>> &storeN,
    vector<vector<int>> &histPerThread,
    int maxIter,
    int width, int height,
    double x_min, double x_max,
    double y_min, double y_max)
{
    omp_set_num_threads(omp_get_max_threads());

#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto &localHist = histPerThread[tid];

#pragma omp for
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                double real = x_min + (double)x / width * (x_max - x_min);
                double imag = y_min + (double)y / height * (y_max - y_min);

                complex<double> z(0, 0);
                complex<double> c(real, imag);

                int n = 0;
                while (abs(z) <= 2.0 && n < maxIter)
                {
                    z = z * z + c;
                    n++;
                }

                storeN[y][x] = n;
                localHist[n]++;
            }
        }
    }
}

// ----------------------------------------------------------
// MERGE HISTOGRAMS
// ----------------------------------------------------------
vector<int> computeHistogram(
    const vector<vector<int>> &histPerThread,
    int maxIter)
{
    int nThreads = histPerThread.size();
    vector<int> hist(maxIter + 1, 0);

    for (int t = 0; t < nThreads; t++)
        for (int i = 0; i <= maxIter; i++)
            hist[i] += histPerThread[t][i];

    return hist;
}

// ----------------------------------------------------------
// COMPUTE CDF
// ----------------------------------------------------------
vector<double> computeCDF(const vector<int> &hist, long totalPixels)
{
    int maxIter = hist.size() - 1;
    vector<double> cdf(maxIter + 1);
    long accum = 0;

    for (int i = 0; i <= maxIter; i++)
    {
        accum += hist[i];
        cdf[i] = double(accum) / totalPixels;
    }
    return cdf;
}

// ----------------------------------------------------------
// COLORIZE FRAME USING CDF + HSV
// ----------------------------------------------------------
void colorizeImage(
    Mat &image,
    const vector<vector<int>> &storeN,
    const vector<double> &cdf,
    int maxIter)
{
    int width = image.cols;
    int height = image.rows;

    double gamma = 0.5; // lightning

#pragma omp parallel for
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int n = storeN[y][x];

            if (n == maxIter)
            {
                image.at<Vec3b>(y, x) = Vec3b(0, 0, 0);
                continue;
            }

            double v = cdf[n];
            double vGamma = pow(v, gamma); // application of gamma
            int V = int(255 * vGamma);     // intensity

            int H = 110; // blue
            int S = 255; // max saturation

            Mat hsv(1, 1, CV_8UC3, Scalar(H, S, V));
            Mat rgb;
            cvtColor(hsv, rgb, COLOR_HSV2BGR);

            image.at<Vec3b>(y, x) = rgb.at<Vec3b>(0, 0);
        }
    }
}

// ----------------------------------------------------------
// WRITE FRAME (PNG)
// ----------------------------------------------------------
void writeFrame(const Mat &img, int frame)
{
    char filename[256];
    sprintf(filename, "images/frame_%05d.png", frame);
    imwrite(filename, img);
}

// ----------------------------------------------------------
// RENDER ONE FRAME (HIGH-LEVEL FUNCTION)
// ----------------------------------------------------------
void computeFrame(
    int width, int height,
    double centerX, double centerY,
    double xRangeStart, double yRangeStart,
    double &zoom,
    Mat &image)
{
    double scale = 1.0 / zoom;
    double xRange = xRangeStart * scale;
    double yRange = yRangeStart * scale;

    double x_min = centerX - xRange / 2.0;
    double x_max = centerX + xRange / 2.0;
    double y_min = centerY - yRange / 2.0;
    double y_max = centerY + yRange / 2.0;

    int maxIter = 64 + int(log2(zoom) * 64);

    int nThreads = omp_get_max_threads();

    vector<vector<int>> storeN(height, vector<int>(width, 0));
    vector<vector<int>> histPerThread(nThreads, vector<int>(maxIter + 1, 0));

    // Compute escape counts & histogram
    computeEscapeValues(storeN, histPerThread, maxIter,
                        width, height, x_min, x_max, y_min, y_max);

    // Merge histogram
    vector<int> hist = computeHistogram(histPerThread, maxIter);

    // Compute CDF
    vector<double> cdf = computeCDF(hist, long(width) * height);

    // Colorize frame
    colorizeImage(image, storeN, cdf, maxIter);

    // Save PNG
    static int frameCount = 0;
    writeFrame(image, frameCount);
    frameCount++;
}

// ----------------------------------------------------------
// MAIN
// ----------------------------------------------------------
int main(int argc, char *argv[])
{
    if (argc > 5)
    {
        fprintf(stderr, "Usage : %s [<width>] [<height>] [<fps>] [<zoomEnd>]\n", argv[0]);
        exit(1);
    }

    const int width = (argc > 1 ? atoi(argv[1]) : 1920);
    const int height = (argc > 2 ? atoi(argv[2]) : 1080);
    const int fps = (argc > 3 ? atoi(argv[3]) : 30);
    const double zoomEnd = (argc > 4 ? atof(argv[4]) : 1e6);

    double centerX = -0.74364388703715870475;
    double centerY = 0.13182590420531197049;

    double aspect = double(width) / height;
    double xRangeStart = 3.0;
    double yRangeStart = xRangeStart / aspect;

    int ret = system("mkdir -p images");
    if (ret != 0)
        printf("Error while creating the folder images\n");

    ret = system("rm images/*.png 2>/dev/null");
    if (ret != 0)
        printf("Error while cleaning images/\n");

    Mat image(height, width, CV_8UC3);

    // --- ZOOM SETTINGS ---
    double zoom = 1.0;
    double secondsPerZoomDoubling = 1.25;
    double zoomScalePerSecond = pow(2.0, 1.0 / secondsPerZoomDoubling);
    double scalePerFrame = pow(zoomScalePerSecond, 1.0 / fps);

    int frame = 0;

    // --- START TIMER ---
    auto timeStart = chrono::high_resolution_clock::now();

    // --- RENDER LOOP ---
    while (zoom < zoomEnd)
    {
        zoom *= scalePerFrame;

        computeFrame(width, height,
                     centerX, centerY,
                     xRangeStart, yRangeStart,
                     zoom,
                     image);

        cout << "Frame " << frame << " | zoom = " << zoom << "\r" << flush;
        frame++;
    }

    // --- STOP TIMER ---
    auto timeEnd = chrono::high_resolution_clock::now();
    double elapsedSec = chrono::duration<double>(timeEnd - timeStart).count();

    cout << "\nEncoding video...\n";

    char cmd[512];
    sprintf(cmd,
            "ffmpeg -y -framerate %d -i images/frame_%%05d.png "
            "-c:v libx264 -pix_fmt yuv420p mandelbrot_zoom.mp4",
            fps);

    ret = system(cmd);
    if (ret != 0)
        printf("Error while creating the video\n");

    // --- FINAL REPORT ---
    cout << "\n====== FINAL STATS ======\n";
    cout << "Frames generated : " << frame << "\n";
    cout << "Final zoom       : " << zoom << "\n";
    cout << "Center X         : " << centerX << "\n";
    cout << "Center Y         : " << centerY << "\n";
    cout << "Total time       : " << elapsedSec << " seconds\n";
    cout << "Time per frame   : " << (elapsedSec / frame) << " seconds\n";
    cout << "=========================\n\n";

    cout << "Done.\n";

    return 0;
}
