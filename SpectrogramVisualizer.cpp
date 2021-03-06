#include "SpectrogramVisualizer.hpp"
#include "QDebug"
#define EPSILON -1e-10f
#include "display_defines.h"
/* static member declarations and initializations */
const char* const SpectrogramVisualizer::noteNames[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb",
                                                        "B"};




void SpectrogramVisualizer::display() {
    //    plotTimeDomain();
    plotSpectralMagnitude();
    plotSpectrogram();
    displayText();
}





const struct SpectrogramVisualizer::shortcuts SpectrogramVisualizer::KEYBOARD_SHORTCUTS = {
    27,   /* ESCAPE */
    'q',  /* QUIT */
    'd',  /* DIAGNOSE */
    ' ',  /* PAUSE */
    ']',  /* SAMPLE_RATE_UP */
    '[',  /* SAMPLE_RATE_DOWN */
    'i'   /* CHANGE_COLOR_SCHEME */
};
const float SpectrogramVisualizer::MIDDLE_C_FREQUENCY = 261.626f;
const unsigned int SpectrogramVisualizer::N_SEMITONES_PER_OCTAVE = 12;

SpectrogramVisualizer::SpectrogramVisualizer(int scrollFactor) {
    isPaused = false;
    colorScale[0] = 100.0f;     // 8-bit intensity offset
    colorScale[1] = 255/120.0f;     // 8-bit intensity slope (per dB units)
    audioInput = new ALSA();
    unsigned int spectrogramSize = audioInput->getSpectrogramSize();
    spectrogramBytes = new char[spectrogramSize];
    spectrogramFloat = new float[spectrogramSize];
    hzPerPixel = 1.0F/(audioInput->getFftLength()*audioInput->getSamplingPeriod());
    highestFrequency = hzPerPixel*AudioInput::N_FREQUENCIES;
    //    highestFrequency = 20000;





    fpsTick = time(NULL);
    frameCount = 0;
    fps = 0;
    colorMode = 2;
    this->scrollFactor = scrollFactor;
    scrollCount = 0;
    gettimeofday(&startTime, NULL);
    runTime = 0.0;
    frequencyReadOff = 0;
    diagnose = 0;
    strcpy(diagnosis, "no diagnosis ...");
}

SpectrogramVisualizer::~SpectrogramVisualizer() {
    delete audioInput;
    delete[] spectrogramBytes;
    delete[] spectrogramFloat;
}



void SpectrogramVisualizer::plotTimeDomain() {
    float tshow = TIME_DOM_START;    // only show the most recent tshow secs of samples
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_LINE_SMOOTH);
    glLineWidth(2);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix(); // use modelview matrix to transform [-tshow,0]x[-1,1] somewhere
    glTranslatef(TIME_XSHIFT,TIME_YSHIFT,TIME_ZSHIFT);
    glScalef(TIME_XSCALE,TIME_YSCALE,TIME_ZSCALE);  // x-scale for time-units, y-scale is 1

    /* draw axes */
    char xLabel[] = "t(s)", yLabel[] = "";
    drawAxes(-tshow, 0, TIME_MIN, TIME_MAX, 1, 1, xLabel, yLabel);

    /* draw time domain signal */
    glColor4f(0.4, 1.0, 0.6, 1);
    glBegin(GL_LINE_STRIP);
    float x = -tshow;
    int bufferIndex = audioInput->getBufferIndex();
    int ilo = bufferIndex-(int) (tshow*audioInput->getSamplingRate());
    int ihi = bufferIndex; // NB get now since capture thread may change it!
    /* plot the most recent piece of buffer */
    float* audioBuffer = audioInput->getAudioBuffer();
    float samplingPeriod = audioInput->getSamplingPeriod();
    for (int i = ilo; i<ihi; i++) {
        glVertex2f(x, audioBuffer[AudioInput::mod(i, audioInput->getBufferSizeSamples())]);
        x += samplingPeriod;  /* time increment */
    }
    glEnd();
    glPopMatrix();
}



//! "real time" fft
void SpectrogramVisualizer::plotSpectralMagnitude() {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix(); // use modelview matrix to transform [-tshow,0]x[-1,1] somewhere
    glTranslatef(REALFFT_XSHIFT, REALFFT_YSHIFT, 0);
    glScalef(REALFFT_MAX_X_FACTOR/highestFrequency, REALFFT_MAXY, REALFFT_MAXZ);  // x-scale for freq-units, y-scale for dB

    //

    glLineWidth(1);

    /* lines showing spectrogram color range */
    glColor4f(0.5, 0.4, 0.2, 1);
    glBegin(GL_LINES);
    //    float dB_min = -colorScale[0]/colorScale[1];
    //    float dB_max = (255.0f-colorScale[0])/colorScale[1];
    float dB_min = REALFFT_MARKER_MIN_DB;
    float dB_max = REALFFT_MARKER_MAX_DB;

    glVertex2f(0, dB_min);
    glVertex2f(highestFrequency, dB_min);
    glVertex2f(0, dB_max);
    glVertex2f(highestFrequency, dB_max);
    glEnd();

    //!!!!! This is where is seems to all happen for the Spectrogram in the bottom left
    /* plot all of the values */
    glColor4f(1.0, 0.8, 0.3, 1);   // the plotTimeDomain
    glBegin(GL_LINE_STRIP);
    float* spectrogramSlice = audioInput->getSpectrogramSlice();


    for (int i = 0; i<AudioInput::N_FREQUENCIES; i++)
        glVertex2f(i*hzPerPixel, 20*(float) log10(spectrogramSlice[i]));
    glEnd();




    /* axis labels */

    drawAxes(EPSILON, highestFrequency,
             SPECTROGRAM_AXIS_MIN_DB,
             SPECTROGRAM_AXIS_MAX_DB,
             REALFFT_MARKER_GRADATION_X,
             REALFFT_MARKER_GRADATION_Y,
             toCharP(LABEL_X),
             toCharP(LABEL_Y));

    glPopMatrix();
}
//! top scrolling spectrogram
void SpectrogramVisualizer::plotSpectrogram() {
    float x0 = SPECTROGRAM_XSHIFT, y0 = SPECTROGRAM_YSHIFT;  /* bot-left location in viewport (as unit square) */
    float curFrequency, lineFrequency;
    float secondsPerPixel = scrollFactor/60.0f; // (float)fps, or longer FPS mean?
    float endTime = secondsPerPixel*AudioInput::N_TIME_WINDOWS;
    char buffer[50];  /* for frequencyReadOff */
    int nHarmonics, i, noteNum, octave;   // for frequencyReadOff

    /* plot the spectrogram values */
    glRasterPos2f(x0, y0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    if (colorMode<2) {
        /* plot B/w */
        glDrawPixels(AudioInput::N_TIME_WINDOWS, AudioInput::N_FREQUENCIES, GL_LUMINANCE, GL_UNSIGNED_BYTE, spectrogramBytes);
    }
    else {
        /* plot RGB */
        glDrawPixels(AudioInput::N_TIME_WINDOWS, AudioInput::N_FREQUENCIES, GL_RGB, GL_UNSIGNED_BYTE_3_3_2, spectrogramBytes);
    }

    /* align spectrogram with the time and frequency axes */
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(x0+(AudioInput::N_TIME_WINDOWS-runTime/secondsPerPixel)/viewportSize[0]
            , y0,
            0);
    glScalef(1.0F/(viewportSize[0]*secondsPerPixel),
            1.0F/(viewportSize[1]*hzPerPixel),
            1);

    /* plot axes */
    char xLabel[] = "t(s)", yLabel[] = "f(Hz)";


    drawAxes(runTime-endTime, runTime, EPSILON, highestFrequency, SPECTROGRAM_AXIS_X_GRADATION_FACTOR
             , SPECTROGRAM_AXIS_Y_GRADATION_FACTOR
             , xLabel, yLabel);



    /* plot frequency read-off line(s) */
    if (frequencyReadOff) {
        /* obtain the selected frequency from current y mouse position */
        curFrequency = hzPerPixel*(viewportSize[1]*(1-y0)-mouseHandle[1]);
        if (curFrequency>0.0) {       // only show if meaningful freq
            nHarmonics = (frequencyReadOff>1) ? 10 : 1;
            /* plot desired frequency line and potentially also its harmonics */
            for (i = 1; i<=nHarmonics; ++i) {
                lineFrequency = i*curFrequency;
                if (i==1) {
                    /* lighter orange for the chosen frequency */
                    glColor4f(0.8, 0.5, 0.0, 1);
                }
                else {
                    /* darker orange for the harmonics */
                    glColor4f(0.5, 0.3, 0.0, 1);
                }

                glDisable(GL_LINE_SMOOTH);
                glLineWidth(1);
                glBegin(GL_LINES);
                glVertex2f(runTime-endTime, lineFrequency);
                glVertex2f(runTime, lineFrequency);
                glEnd();

                /* draw text label(s) */
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                noteNum = (int) roundf(N_SEMITONES_PER_OCTAVE*logf(lineFrequency/MIDDLE_C_FREQUENCY)/logf(2.0));
                octave = 4+noteNum/N_SEMITONES_PER_OCTAVE;  /* note: int division intentional */
                if (octave<0) octave = (int) NAN;
                sprintf(buffer, "  %.d   %s%d", (int) roundf(lineFrequency), noteNames[(noteNum+1200)%N_SEMITONES_PER_OCTAVE],
                        octave);
                display::smallText(runTime-endTime, lineFrequency+3.0f*hzPerPixel, buffer);
            }
        }
    }
    glPopMatrix();
}

void SpectrogramVisualizer::drawAxes(float xStart, float xEnd, float yStart, float yEnd,
                                     float xFudgeFactor, float yFudgeFactor, char* xLabel, char* yLabel) {
    int nTicks, i;
    char label[20];
    float ticks[100];
    float xPixelSize = 2.0f*(xEnd-xStart)/(viewportSize[0]*xFudgeFactor);
    float yPixelSize = 2.0f*(yEnd-yStart)/(viewportSize[1]*yFudgeFactor);

    /* draw axis lines */
    glColor4f(0.7, 1.0, 1.0, 1);
    glDisable(GL_LINE_SMOOTH);
    glLineWidth(1);
    glBegin(GL_LINE_STRIP);
    glVertex2f(xStart, yEnd);
    glVertex2f(xStart, yStart);
    glVertex2f(xEnd, yStart);
    glEnd();

    /* draw axis labels */
    int mod = 8;
    display::smallText(xEnd+mod*xPixelSize, yStart-mod*yPixelSize, xLabel);
    display::smallText(xStart-mod*xPixelSize, yEnd+mod*yPixelSize, yLabel);

    /* draw x axis ticks */
    nTicks = chooseTics(xStart, xEnd-xStart, xFudgeFactor, ticks);
    float topTickX = yStart;
    float bottomTickX = topTickX-0.02f*(yEnd-yStart)/yFudgeFactor;
    glBegin(GL_LINES);
    for (i = 0; i<nTicks; ++i) {
        glVertex2d(ticks[i], bottomTickX);
        glVertex2d(ticks[i], topTickX);
    }
    glEnd();

    /* draw x axis values */
    for (i = 0; i<nTicks; ++i) {
        sprintf(label, "%.6g", ticks[i]);
        display::smallText((ticks[i]-4*strlen(label)*xPixelSize), (bottomTickX-12*yPixelSize), label);
    }

    /* draw y axis ticks */
    nTicks = chooseTics(yStart, yEnd-yStart, yFudgeFactor, ticks);
    float topTickY = xStart;
    float bottomTickY = topTickY-0.02f*(xEnd-xStart)/xFudgeFactor;
    glBegin(GL_LINES);
    for (i = 0; i<nTicks; ++i) {
        glVertex2d(bottomTickY, ticks[i]);
        glVertex2d(topTickY, ticks[i]);
    }
    glEnd();

    /* draw y axis values */
    for (i = 0; i<nTicks; ++i) {
        sprintf(label, "%.6g", ticks[i]);
        display::smallText((bottomTickY-8*strlen(label)*xPixelSize), (ticks[i]-4*yPixelSize), label);
    }
}

char SpectrogramVisualizer::colorByteMap(float x) {
    float fac = 20.0*colorScale[1]; // colorScale[1] units: intensity/dB
    int k = (int) (colorScale[0]+fac*log10(x));
    if (k>255) k = 255; else if (k<0) k = 0;

    if (colorMode==1) {
        // inverse b/w
        return (char) (255-k);
    }
    else if (colorMode==2) {    // color (pack into 3_3_2 RGB format) .. SLOW?
        float a = k/255.0f;       // 0<a<1. now map from [0,1] to rgb in [0,1]
        float r = 5*(a-0.2);
        if (r<0) r = 0.0; else if (r>=1) r = .955; // clip
        float g = 5*(a-0.6);
        if (g<0) g = 0.0; else if (g>=1) g = .995;
        float b = 5*a;
        if (a>0.8) b = 5*(a-0.8); else if (a>0.4) b = 5*(0.6-a);
        if (b<0) b = 0.0; else if (b>=1) b = .995;
        return (char) (b*4+4*((int) (g*8))+32*((int) (r*8)));  // pack to 8-bits
    }
    else {
        // b/w
        return (char) k;
    }
}

void SpectrogramVisualizer::recomputeSpectrogramBytes() {
    int i, j, n = AudioInput::N_TIME_WINDOWS;
    for (i = 0; i<n; ++i)
        for (j = 0; j<audioInput->N_FREQUENCIES; ++j)
            spectrogramBytes[j*n+i] = colorByteMap(spectrogramFloat[j*n+i]);
}

int SpectrogramVisualizer::chooseTics(float lowValue, float range, float fudgeFactor, float* tickMarks) {
    int i, nTics, startTick;
    float exponent, logr, spacing;
    if (fudgeFactor==0.0) fudgeFactor = 1.0;

    /* adjust the range multiplier here to give good tic density... */
    logr = (float) log10(range*0.4/fudgeFactor);
    exponent = (float) floor(logr);
    spacing = (float) pow(10.0, exponent);
    if (logr-exponent>log10(5.0))
        spacing *= 5.0;
    else if (logr-exponent>log10(2.0))
        spacing *= 2.0;

    /* (int) and copysign trick is to convert the floor val to an int... */
    startTick = (int) (copysign(0.5, lowValue)+1.0+floor(lowValue/spacing));

    nTics = (int) (1.0+(lowValue+range-startTick*spacing)/spacing);
    for (i = 0; i<nTics; ++i) {
        tickMarks[i] = spacing*(startTick+i);
    }
    return nTics;
}

void SpectrogramVisualizer::scrollSpectrogram() {
    int i, j;
    int n = AudioInput::N_TIME_WINDOWS;
    float* newSpectrogramData = audioInput->getSpectrogramSlice();

    /* scroll existing data */
    for (i = 0; i<n; ++i) {
        for (j = 0; j<AudioInput::N_FREQUENCIES; ++j) {
            spectrogramFloat[j*n+i] = spectrogramFloat[j*n+i+1];
            spectrogramBytes[j*n+i] = spectrogramBytes[j*n+i+1];
        }
    }

    /* add new data */
    for (j = 0; j<AudioInput::N_FREQUENCIES; ++j) {
        spectrogramFloat[j*n+n-1] = newSpectrogramData[j];
        spectrogramBytes[j*n+n-1] = colorByteMap(newSpectrogramData[j]);
    }
}


void SpectrogramVisualizer::displayText() {
    glEnable(GL_BLEND); // glBlendFunc (GL_ONE,GL_ZERO); // overwrite entirely
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glColor4f(1, .4, .4, 1.0); // write FPS, other params... coords in unit square
    char str[50];
    sprintf(str, "%d FPS", fps);
    display::smallText(0.92, 0.96, str);
    sprintf(str, "gain offset %.0f dB", colorScale[0]);
    display::smallText(0.00, 0.04, str);
    sprintf(str, "dyn range  %.1f dB", 255.0/colorScale[1]);
    display::smallText(0.00, 0.02, str);

    if (diagnose) {  // show diagnosis
        glColor4f(.2, .2, .2, 0.8);  // transparent box
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix(); // use modelview matrix to transform unit square to text box
        glTranslatef(0.5, 0.5, 0);
        glScalef(0.5, 0.4, 1);
        glTranslatef(-0.5f, -0.5f, 0);
        glBegin(GL_POLYGON);
        glVertex2f(0, 0);
        glVertex2f(1, 0);
        glVertex2f(1, 1);
        glVertex2f(0, 1);
        glVertex2f(0, 0); // unit square
        glEnd();
        glColor4f(1, 1, 1, 1);                  // text
        display::largeText(0.2, 0.5, diagnosis); // coords relative to box as unit sq
        glPopMatrix();
    }
}

void SpectrogramVisualizer::idle() {
    ++frameCount;        // compute FPS...
    time_t now = time(NULL);     // integer in seconds
    if (now>fpsTick) {    // advanced by at least 1 sec?
        fps = frameCount;
        frameCount = 0;
        fpsTick = now;
    }

    /* if not paused, scroll every scrollFactor vSyncs */
    if (!audioInput->isPause()) {
        timeval nowe;  // update runtime
        gettimeofday(&nowe, NULL);
        runTime += 1/60.0;        // is less jittery, approx true
        runTime = (float) fmod(runTime, 100.0);    // wrap time label after 100 secs

        ++scrollCount;
        if (scrollCount==SpectrogramVisualizer::scrollFactor) {
            scrollSpectrogram();
            scrollCount = 0;
        }
    }
}

void SpectrogramVisualizer::pause() {
    isPaused = !isPaused;
    audioInput->setPause(!audioInput->isPause());
}

void SpectrogramVisualizer::keyboard(unsigned char key, int xPos, int yPos) {
    if (key==KEYBOARD_SHORTCUTS.ESCAPE || key==KEYBOARD_SHORTCUTS.QUIT) {  // esc or q to quit
        audioInput->quitNow();
        exit(0);
    }
    else if (key==KEYBOARD_SHORTCUTS.PAUSE) {
        pause();
    }
    else if (key==KEYBOARD_SHORTCUTS.DIAGNOSE) {
        diagnose = !diagnose;         // toggle text overlay
    }
    else if (key==KEYBOARD_SHORTCUTS.SAMPLE_RATE_UP) {               // speed up scroll samplingRate
        if (SpectrogramVisualizer::scrollFactor>1) {
            SpectrogramVisualizer::scrollFactor--;
            scrollCount = 0;
        }
    }
    else if (key==KEYBOARD_SHORTCUTS.SAMPLE_RATE_DOWN) {
        if (SpectrogramVisualizer::scrollFactor<50) SpectrogramVisualizer::scrollFactor++;
    }
    else if (key==KEYBOARD_SHORTCUTS.CHANGE_COLOR_SCHEME) {
        colorMode = (colorMode+1)%3;     // spectrogram color scheme
        recomputeSpectrogramBytes();
    }
    else {
        fprintf(stderr, "pressed key %d\n", (int) key);
    }
}

void SpectrogramVisualizer::special(int key, int xPos, int yPos) {
    if (key==102) { // rt
        colorScale[1] *= 1.5;
        recomputeSpectrogramBytes(); // contrast
    }
    else if (key==100) { // lt
        colorScale[1] /= 1.5;
        recomputeSpectrogramBytes(); // contrast
    }
    else if (key==103) { // dn
        colorScale[0] -= 20;
        recomputeSpectrogramBytes();  // brightness
    }
    else if (key==101) { // up
        colorScale[0] += 20;
        recomputeSpectrogramBytes();  // brightness
    }
    else {
        fprintf(stderr, "pressed special key %d\n", key);
    }
}

void SpectrogramVisualizer::reshape(int w, int h) {
    viewportSize[0] = w;
    viewportSize[1] = h;
}

void SpectrogramVisualizer::mouse(int button, int state, int x, int y) {
    mouseHandle[0] = x;  // this seems to be needed for correct panning
    mouseHandle[1] = y;
    mouseHandle[2] = button;

    if (button==GLUT_LEFT_BUTTON) {
        qDebug("%d",state);
        frequencyReadOff = state==GLUT_DOWN ? 1 : 0;  /* toggle */
    }
    else if (button==GLUT_RIGHT_BUTTON) {
        frequencyReadOff = state==GLUT_DOWN ? 2 : 0;  /* toggle with harmonics shown */
    }
    else if (state==GLUT_UP) {
        /* GLUT_MIDDE_BUTTON is pressed */
        recomputeSpectrogramBytes();
    }
}

void SpectrogramVisualizer::motion(int x, int y) {
    int dx = (int) (x-mouseHandle[0]);
    int dy = (int) (y-mouseHandle[1]);
    if (mouseHandle[2]==GLUT_MIDDLE_BUTTON) {   // controls color scale
        colorScale[0] += dx/20.0;  // brightness
        colorScale[1] *= exp(-dy/200.0); // contrast
    }
    mouseHandle[0] = x;
    mouseHandle[1] = y;
}
