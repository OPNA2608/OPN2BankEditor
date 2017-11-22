/*
 * OPL Bank Editor by Wohlstand, a free tool for music bank editing
 * Copyright (c) 2016-2017 Vitaly Novichkov <admin@wohlnet.ru>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef IS_QT_4
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#endif
#include <QQueue>
#include <QProgressDialog>

#include <cmath>

#include "measurer.h"

//Measurer is always needs for emulator
#include "Ym2612_Emu.h"

/******************************************************************
 ******************************************************************
 *                  DRAFT
 * THIS MEASURER IS WIP AND IS NOT WORKING AND RESULTS GARBAGE!!!
 *
 ******************************************************************
 ******************************************************************/

struct DurationInfo
{
    uint64_t    peak_amplitude_time;
    double      peak_amplitude_value;
    double      quarter_amplitude_time;
    double      begin_amplitude;
    double      interval;
    double      keyoff_out_time;
    int64_t     ms_sound_kon;
    int64_t     ms_sound_koff;
    bool        nosound;
    uint8_t     padding[7];
};

struct ChipEmulator
{
    Ym2612_Emu opl;
    void setRate(uint32_t rate)
    {
        opl.set_rate(rate, 7670454.0);
    }
    void WRITE_REG(uint8_t port, uint8_t address, uint8_t byte)
    {
        switch(port)
        {
        case 0:
            opl.write0(address, byte);
            break;
        case 1:
            opl.write1(address, byte);
            break;
        }
    }
};

static void MeasureDurations(FmBank::Instrument *in_p)
{
    FmBank::Instrument &in = *in_p;
    std::vector<int16_t> stereoSampleBuf;

    const unsigned rate = 44100;
    const unsigned interval             = 150;
    const unsigned samples_per_interval = rate / interval;
    const int notenum =
        in.percNoteNum < 20 ? (44 + in.percNoteNum) :
                            in.percNoteNum >= 128 ? (44 + 128 - in.percNoteNum) : in.percNoteNum;
    ChipEmulator opl;

    static const uint8_t initdata[(2 + 3 + 2 + 2) * 2] =
    {
        0x004, 96, 0x004, 128,      // Pulse timer
        0x10, 0, 0x10, 1, 0x10, 0, // Pulse OPL3 enable, leave disabled
        0x001, 32, 0x0BD, 0         // Enable wave & melodic
    };

    opl.setRate(rate);

    for(unsigned a = 0; a < 18; a += 2)
        opl.WRITE_REG(0, initdata[a], initdata[a + 1]);

    //const unsigned n_notes = in.en_4op || in.en_pseudo4op ? 2 : 1;
    unsigned x[2] = {0, 0};
    //if(n_notes == 2 && !in.en_pseudo4op)
    //{
        opl.WRITE_REG(0, 0x10, 1);
        opl.WRITE_REG(0, 0x10, 1);
    //}

    uint8_t rawData[2][11];

//    rawData[0][0] = in.getAVEKM(MODULATOR1);
//    rawData[0][1] = in.getAVEKM(CARRIER1);
//    rawData[0][2] = in.getAtDec(MODULATOR1);
//    rawData[0][3] = in.getAtDec(CARRIER1);
//    rawData[0][4] = in.getSusRel(MODULATOR1);
//    rawData[0][5] = in.getSusRel(CARRIER1);
//    rawData[0][6] = in.getWaveForm(MODULATOR1);
//    rawData[0][7] = in.getWaveForm(CARRIER1);
//    rawData[0][8] = in.getKSLL(MODULATOR1);
//    rawData[0][9] = in.getKSLL(CARRIER1);
//    rawData[0][10] = in.getFBConn1();

//    rawData[1][0] = in.getAVEKM(MODULATOR2);
//    rawData[1][1] = in.getAVEKM(CARRIER2);
//    rawData[1][2] = in.getAtDec(MODULATOR2);
//    rawData[1][3] = in.getAtDec(CARRIER2);
//    rawData[1][4] = in.getSusRel(MODULATOR2);
//    rawData[1][5] = in.getSusRel(CARRIER2);
//    rawData[1][6] = in.getWaveForm(MODULATOR2);
//    rawData[1][7] = in.getWaveForm(CARRIER2);
//    rawData[1][8] = in.getKSLL(MODULATOR2);
//    rawData[1][9] = in.getKSLL(CARRIER2);
//    rawData[1][10] = in.getFBConn2();

//    for(unsigned n = 0; n < n_notes; ++n)
//    {
//        static const unsigned char patchdata[11] =
//        {0x20, 0x23, 0x60, 0x63, 0x80, 0x83, 0xE0, 0xE3, 0x40, 0x43, 0xC0};
//        for(unsigned a = 0; a < 10; ++a)
//            WRITE_REG(patchdata[a] + n * 8, rawData[n][a]);
//        WRITE_REG(patchdata[10] + n * 8, rawData[n][10] | 0x30);
//    }

    //for(unsigned n = 0; n < n_notes; ++n)
    {
        double hertz = 172.00093 * std::exp(0.057762265 * (notenum + in.note_offset1));
        if(hertz > 131071)
        {
            std::fprintf(stderr, "MEASURER WARNING: Why does note %d + finetune %d produce hertz %g?          \n",
                         notenum, in.note_offset1, hertz);
            hertz = 131071;
        }
        //x[n] = 0x2000;
        while(hertz >= 1023.5)
        {
            hertz /= 2.0;    // Calculate octave
            //x[n] += 0x400;
        }
        //x[n] += (unsigned int)(hertz + 0.5);

        // Keyon the note
        opl.WRITE_REG(0, 0xA0, 0xFF);
        opl.WRITE_REG(0, 0xB0, 8);
    }

    const unsigned max_on  = 40;
    const unsigned max_off = 60;

    // For up to 40 seconds, measure mean amplitude.
    std::vector<double> amplitudecurve_on;
    double highest_sofar = 0;
    for(unsigned period = 0; period < max_on * interval; ++period)
    {
        stereoSampleBuf.clear();
        stereoSampleBuf.resize(samples_per_interval * 2, 0);

        opl.opl.run(samples_per_interval, stereoSampleBuf.data());

        double mean = 0.0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
            mean += stereoSampleBuf[c * 2];
        mean /= samples_per_interval;
        double std_deviation = 0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
        {
            double diff = (stereoSampleBuf[c * 2] - mean);
            std_deviation += diff * diff;
        }
        std_deviation = std::sqrt(std_deviation / samples_per_interval);
        amplitudecurve_on.push_back(std_deviation);
        if(std_deviation > highest_sofar)
            highest_sofar = std_deviation;

        if(period > 6 * interval && std_deviation < highest_sofar * 0.2)
            break;
    }

    // Keyoff the note
    //for(unsigned n = 0; n < n_notes; ++n)
        opl.WRITE_REG(0, 0xB0 * 3, 8 & 0xDF);

    // Now, for up to 60 seconds, measure mean amplitude.
    std::vector<double> amplitudecurve_off;
    for(unsigned period = 0; period < max_off * interval; ++period)
    {
        stereoSampleBuf.clear();
        stereoSampleBuf.resize(samples_per_interval * 2);

        opl.opl.run(samples_per_interval, stereoSampleBuf.data());

        double mean = 0.0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
            mean += stereoSampleBuf[c * 2];
        mean /= samples_per_interval;
        double std_deviation = 0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
        {
            double diff = (stereoSampleBuf[c * 2] - mean);
            std_deviation += diff * diff;
        }
        std_deviation = std::sqrt(std_deviation / samples_per_interval);
        amplitudecurve_off.push_back(std_deviation);

        if(std_deviation < highest_sofar * 0.2)
            break;
    }

    /* Analyze the results */
    double begin_amplitude        = amplitudecurve_on[0];
    double peak_amplitude_value   = begin_amplitude;
    size_t peak_amplitude_time    = 0;
    size_t quarter_amplitude_time = amplitudecurve_on.size();
    size_t keyoff_out_time        = 0;

    for(size_t a = 1; a < amplitudecurve_on.size(); ++a)
    {
        if(amplitudecurve_on[a] > peak_amplitude_value)
        {
            peak_amplitude_value = amplitudecurve_on[a];
            peak_amplitude_time  = a;
        }
    }
    for(size_t a = peak_amplitude_time; a < amplitudecurve_on.size(); ++a)
    {
        if(amplitudecurve_on[a] <= peak_amplitude_value * 0.2)
        {
            quarter_amplitude_time = a;
            break;
        }
    }
    for(size_t a = 0; a < amplitudecurve_off.size(); ++a)
    {
        if(amplitudecurve_off[a] <= peak_amplitude_value * 0.2)
        {
            keyoff_out_time = a;
            break;
        }
    }

    if(keyoff_out_time == 0 && amplitudecurve_on.back() < peak_amplitude_value * 0.2)
        keyoff_out_time = quarter_amplitude_time;

    DurationInfo result;
    result.peak_amplitude_time = peak_amplitude_time;
    result.peak_amplitude_value = peak_amplitude_value;
    result.begin_amplitude = begin_amplitude;
    result.quarter_amplitude_time = (double)quarter_amplitude_time;
    result.keyoff_out_time = (double)keyoff_out_time;

    result.ms_sound_kon  = (int64_t)(quarter_amplitude_time * 1000.0 / interval);
    result.ms_sound_koff = (int64_t)(keyoff_out_time        * 1000.0 / interval);
    result.nosound = (peak_amplitude_value < 0.5);

    in.ms_sound_kon = (uint16_t)result.ms_sound_kon;
    in.ms_sound_koff = (uint16_t)result.ms_sound_koff;
}


Measurer::Measurer(QWidget *parent) :
    QObject(parent),
    m_parentWindow(parent)
{}

Measurer::~Measurer()
{}

bool Measurer::doMeasurement(FmBank &bank, FmBank &bankBackup)
{
    QQueue<FmBank::Instrument *> tasks;

    int i = 0;
    for(i = 0; i < bank.Ins_Melodic_box.size() && i < bankBackup.Ins_Melodic_box.size(); i++)
    {
        FmBank::Instrument &ins1 = bank.Ins_Melodic_box[i];
        FmBank::Instrument &ins2 = bankBackup.Ins_Melodic_box[i];
        if((ins1.ms_sound_kon == 0) || (memcmp(&ins1, &ins2, sizeof(FmBank::Instrument)) != 0))
            tasks.enqueue(&ins1);
    }
    for(; i < bank.Ins_Melodic_box.size(); i++)
        tasks.enqueue(&bank.Ins_Melodic_box[i]);

    for(i = 0; i < bank.Ins_Percussion_box.size() && i < bankBackup.Ins_Percussion_box.size(); i++)
    {
        FmBank::Instrument &ins1 = bank.Ins_Percussion_box[i];
        FmBank::Instrument &ins2 = bankBackup.Ins_Percussion_box[i];
        if((ins1.ms_sound_kon == 0) || (memcmp(&ins1, &ins2, sizeof(FmBank::Instrument)) != 0))
            tasks.enqueue(&ins1);
    }
    for(; i < bank.Ins_Percussion_box.size(); i++)
        tasks.enqueue(&bank.Ins_Percussion_box[i]);

    if(tasks.isEmpty())
        return true;// Nothing to do! :)

    QProgressDialog m_progressBox(m_parentWindow);
    m_progressBox.setWindowModality(Qt::WindowModal);
    m_progressBox.setWindowTitle(tr("Sounding delay calculaion"));
    m_progressBox.setLabelText(tr("Please wait..."));

    #ifndef IS_QT_4
    QFutureWatcher<void> watcher;
    watcher.connect(&m_progressBox, SIGNAL(canceled()), &watcher, SLOT(cancel()));
    watcher.connect(&watcher, SIGNAL(progressRangeChanged(int,int)), &m_progressBox, SLOT(setRange(int,int)));
    watcher.connect(&watcher, SIGNAL(progressValueChanged(int)), &m_progressBox, SLOT(setValue(int)));

    watcher.setFuture(QtConcurrent::map(tasks, &MeasureDurations));

    m_progressBox.exec();
    watcher.waitForFinished();

    tasks.clear();
    return !watcher.isCanceled();

    #else
    m_progressBox.setMaximum(tasks.size());
    m_progressBox.setValue(0);
    int count = 0;
    foreach(FmBank::Instrument *ins, tasks)
    {
        MeasureDurations(ins);
        m_progressBox.setValue(++count);
        if(m_progressBox.wasCanceled())
            return false;
    }
    return true;
    #endif
}

bool Measurer::doMeasurement(FmBank::Instrument &instrument)
{
    QProgressDialog m_progressBox(m_parentWindow);
    m_progressBox.setWindowModality(Qt::WindowModal);
    m_progressBox.setWindowTitle(tr("Sounding delay calculaion"));
    m_progressBox.setLabelText(tr("Please wait..."));

    #ifndef IS_QT_4
    QFutureWatcher<void> watcher;
    watcher.connect(&m_progressBox, SIGNAL(canceled()), &watcher, SLOT(cancel()));
    watcher.connect(&watcher, SIGNAL(progressRangeChanged(int,int)), &m_progressBox, SLOT(setRange(int,int)));
    watcher.connect(&watcher, SIGNAL(progressValueChanged(int)), &m_progressBox, SLOT(setValue(int)));

    watcher.setFuture(QtConcurrent::run(&MeasureDurations, &instrument));

    m_progressBox.exec();
    watcher.waitForFinished();

    return !watcher.isCanceled();

    #else
    m_progressBox.show();
    MeasureDurations(&instrument);
    return true;
    #endif
}
