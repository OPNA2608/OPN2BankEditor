/*
 * OPL Bank Editor by Wohlstand, a free tool for music bank editing
 * Copyright (c) 2018 Vitaly Novichkov <admin@wohlnet.ru>
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

#include "generator_realtime.h"
#include "generator.h"
#include <chrono>
#include <string.h>

enum MessageTag
{
    MSG_MidiEvent,
    MSG_CtlSilence,
    MSG_CtlNoteOffAllChans,
    MSG_CtlPlayNote,
    MSG_CtlStopNote,
    MSG_CtlPlayChord,
    MSG_CtlPatchChange,
    MSG_CtlLFO,
    MSG_CtlLFOFreq,
};

struct MessageHeader
{
    MessageTag tag;
    unsigned size;
};

// Begin Messages
enum class ChordType
{
    Major, Minor,
    Augmented, Diminished,
    Major7, Minor7,
};

struct ChordMessage
{
    ChordType chord;
    unsigned note;
};

struct PatchChangeMessage
{
    FmBank::Instrument instrument;
    bool isDrum;
};
// End Messages

enum { fifo_capacity = 8192 };

static void wait_for_fifo_write_space(Ring_Buffer &rb, unsigned size)
{
    while(rb.size_free() < sizeof(MessageHeader) + size) {
#if defined(_WIN32)
        Sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
}

IRealtimeControl::IRealtimeControl(QObject *parent)
    : QObject(parent)
{
    m_debugInfoTimer = new QTimer(this);
    m_debugInfoTimer->setInterval(50);
    connect(m_debugInfoTimer, SIGNAL(timeout()), this, SLOT(debugInfoUpdate()));
    m_debugInfoTimer->start();
}

void IRealtimeControl::debugInfoUpdate()
{
    GeneratorDebugInfo info = generatorDebugInfo();
    emit debugInfo(info.toStr());
}

RealtimeGenerator::RealtimeGenerator(const std::shared_ptr<Generator> &gen, QObject *parent)
    : IRealtimeControl(parent),
      m_gen(gen),
      m_rb_ctl(new Ring_Buffer(fifo_capacity)),
      m_rb_midi(new Ring_Buffer(fifo_capacity)),
      m_body(new uint8_t[fifo_capacity])
{
}

RealtimeGenerator::~RealtimeGenerator()
{}

/* Control */
void RealtimeGenerator::ctl_switchChip(int chipId)
{
    // non-RT, hence lock and processing in control thread
    std::unique_lock<mutex_type> lock(m_generator_mutex);
    m_gen->switchChip((Generator::OPN_Chips)chipId);
}

void RealtimeGenerator::ctl_silence()
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlSilence, 0};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
}

void RealtimeGenerator::ctl_noteOffAllChans()
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlNoteOffAllChans, 0};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
}

void RealtimeGenerator::ctl_playNote()
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlPlayNote, sizeof(uint)};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
    rb.put(m_note);
}

void RealtimeGenerator::ctl_stopNote()
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlStopNote, sizeof(uint)};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
    rb.put(m_note);
}

void RealtimeGenerator::ctl_playChord(int chord)
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlPlayChord, sizeof(ChordMessage)};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
    ChordMessage ch;
    ch.chord = (ChordType)chord;
    ch.note = m_note;
    rb.put(ch);
}

void IRealtimeControl::ctl_playMajorChord()
    { ctl_playChord((int)ChordType::Major); }
void IRealtimeControl::ctl_playMinorChord()
    { ctl_playChord((int)ChordType::Minor); }
void IRealtimeControl::ctl_playAugmentedChord()
    { ctl_playChord((int)ChordType::Augmented); }
void IRealtimeControl::ctl_playDiminishedChord()
    { ctl_playChord((int)ChordType::Diminished); }
void IRealtimeControl::ctl_playMajor7Chord()
    { ctl_playChord((int)ChordType::Major7); }
void IRealtimeControl::ctl_playMinor7Chord()
    { ctl_playChord((int)ChordType::Minor7); }

void RealtimeGenerator::ctl_changePatch(FmBank::Instrument &instrument, bool isDrum)
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlPatchChange, sizeof(PatchChangeMessage)};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
    PatchChangeMessage pc;
    pc.instrument = instrument;
    pc.isDrum = isDrum;
    rb.put(pc);
}

void RealtimeGenerator::ctl_changeLFO(bool lfo)
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlLFO, sizeof(bool)};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
    rb.put(lfo);
}

void RealtimeGenerator::ctl_changeLFOfreq(int freq)
{
    Ring_Buffer &rb = *m_rb_ctl;
    MessageHeader hdr = {MSG_CtlLFO, sizeof(int)};
    wait_for_fifo_write_space(rb, hdr.size);
    rb.put(hdr);
    rb.put(freq);
}

/* MIDI */
void RealtimeGenerator::midi_event(const uint8_t *msg, unsigned msglen)
{
    enum { midi_msglen_max = 64 };

    if(msglen > midi_msglen_max)
        return;

    Ring_Buffer &rb = *m_rb_midi;
    MessageHeader hdr = {MSG_MidiEvent, msglen};
    if (rb.size_free() >= sizeof(hdr) + hdr.size) {
        rb.put(hdr);
        rb.put(msg, msglen);
    }
}

/* Realtime */
void RealtimeGenerator::rt_generate(int16_t *frames, unsigned nframes)
{
    std::unique_lock<mutex_type> lock(m_generator_mutex, std::try_to_lock);
    if(!lock.owns_lock()) {
        memset(frames, 0, 2 * nframes * sizeof(*frames));
        return;
    }

    MessageHeader header;

    /* handle Control messages */
    for(Ring_Buffer &rb = *m_rb_ctl;
         rb.peek(header) && rb.size_used() >= sizeof(header) + header.size;)
    {
        rb.discard(sizeof(header));
        rb.get(m_body.get(), header.size);
        rt_message_process(header.tag, m_body.get(), header.size);
    }

    /* handle MIDI messages */
    for(Ring_Buffer &rb = *m_rb_midi;
         rb.peek(header) && rb.size_used() >= sizeof(header) + header.size;)
    {
        rb.discard(sizeof(header));
        rb.get(m_body.get(), header.size);
        rt_message_process(header.tag, m_body.get(), header.size);
    }

    m_gen->generate(frames, nframes);
}

void RealtimeGenerator::rt_message_process(int tag, const uint8_t *data, unsigned len)
{
    Generator &gen = *m_gen;

    switch(tag) {
    case MSG_MidiEvent:
        rt_midi_process(data, len);
        break;
    case MSG_CtlSilence:
        gen.Silence();
        break;
    case MSG_CtlNoteOffAllChans:
        gen.NoteOffAllChans();
        break;
    case MSG_CtlPlayNote:
        gen.changeNote(*(unsigned *)data);
        gen.PlayNote();
        break;
    case MSG_CtlStopNote:
        gen.changeNote(*(unsigned *)data);
        gen.StopNote();
        break;
    case MSG_CtlPlayChord: {
        ChordMessage msg = *(ChordMessage *)data;
        gen.changeNote(msg.note);
        switch(msg.chord) {
        case ChordType::Major: gen.PlayMajorChord(); break;
        case ChordType::Minor: gen.PlayMinorChord(); break;
        case ChordType::Augmented: gen.PlayAugmentedChord(); break;
        case ChordType::Diminished: gen.PlayDiminishedChord(); break;
        case ChordType::Major7: gen.PlayMajor7Chord(); break;
        case ChordType::Minor7: gen.PlayMinor7Chord(); break;
        }
        break;
    }
    case MSG_CtlPatchChange: {
        const PatchChangeMessage &pc = *(PatchChangeMessage *)data;
        gen.changePatch(pc.instrument, pc.isDrum);
        break;
    }
    case MSG_CtlLFO:
        gen.changeLFO(*(bool *)data);
        break;
    case MSG_CtlLFOFreq:
        gen.changeLFOfreq(*(int *)data);
        break;
    }
}

void RealtimeGenerator::rt_midi_process(const uint8_t *data, unsigned len)
{
    Generator &gen = *m_gen;

    if(len == 3)
    {
        unsigned msg = data[0] >> 4;
        unsigned note = data[1] & 0x7f;
        unsigned vel = data[2] & 0x7f;

        if(msg == 0x9 && vel == 0)
            msg = 0x8;

        switch(msg) {
        case 0x8:
            gen.changeNote((int)note);
            gen.StopNote();
            break;
        case 0x9:
            gen.changeNote((int)note);
            gen.PlayNote();
            break;
        case 0xb:
            if (note == 120)  // all sound off
                gen.Silence();
            if (note == 123)  // all notes off
                gen.NoteOffAllChans();
            break;
        }
    }
}

const GeneratorDebugInfo &RealtimeGenerator::generatorDebugInfo() const
{
    return m_gen->debugInfo();
}
