# G2fresh test pack for your Nord Modular G2

*Version française : [README.fr.md](README.fr.md). To print: README.html.*

Thank you for helping! G2fresh is a new editor for the G2 that can also play patches without a G2. Some things we can only check against a real one. This takes about 20 minutes: you load a few small patches, record them, answer a few questions, and send it all back.

**You need:** a G2, G2X or G2 Engine; Clavia's own editor on a computer that still runs it (you do not need G2fresh); an audio interface with two line inputs (four is better) and anything that records WAV files.

**In this pack:** `patches/` (B01 to B14; they play by themselves, no keyboard needed), `B10 Stimulus.wav` (a test tone for step 3), `form.txt` (your answers; French: `form.fr.txt`).

Nothing is stored in your G2 unless you press Store. Loading a patch replaces what is in the slot (B08 replaces the whole performance), so save your own work first.

## 1. Set up

1. Connect the G2's OUT 1 and OUT 2 to two line inputs of your interface, and OUT 3 and OUT 4 too if you have four inputs free. Straight cables: no mixer, no effects, no EQ.
2. Turn the G2's **Master Level** knob fully up.
3. On the interface, use line inputs. If they have a level switch (+4 dBu / -10 dBV), note its position.
4. Turn your speakers down: B02 and B06 are loud and harsh.
5. Load **B02 Level Steps** (see step 2 for how) and watch your input meters for 15 seconds. Set the input gain so that the loudest moment stays below the top of the meter; all the way down is usually right. **Then do not touch the gain again**, and write its position in the form.
6. Set your recorder to **WAV, 24 bit, 96 kHz** (48 kHz if 96 is not possible). No normalising, no plug-ins, no processing of any kind.

## 2. Record the patches

To load a patch: in Clavia's editor, choose **File > Open** and pick the file (or drag the file onto a slot). The G2's display shows the patch's name, and it starts playing. You can also copy the patches into a bank with the editor and load them from the G2's panel.

For each line of the table: load the patch, wait two seconds, record for the time given, and save the recording under the name given. A stereo file with OUT 1 on the left and OUT 2 on the right is what we want.

| Load | Record | Save as | You should hear |
|---|---|---|---|
| B01 Level Ref | 30 s | B01 Level Ref.wav | a steady tone |
| B02 Level Steps | 40 s | B02 Level Steps.wav | a tone, louder every second, distorted at the end |
| B03 Output Map | 20 s | B03 Output Map.wav | a chord: one note per output |
| B04 Pad | 30 s | B04 Pad.wav | a tone at two levels, then silence |
| B05 White Noise | 30 s | B05 White Noise.wav | hiss |
| B06 Bright Saw | 30 s | B06 Bright Saw.wav | buzzing notes, higher and higher |
| B07 Silence | 15 s | B07 Silence.wav | nothing |
| B08 Clock 120 | 60 s | B08 Clock 120.wav | clicks, two a second on each side |
| B09a Saw Filter | 20 s | B09a Saw Filter.wav | a short bass phrase |
| B09b FM Phrase | 20 s | B09b FM Phrase.wav | a short bell-like phrase |
| B09c Drum | 20 s | B09c Drum.wav | a drum pattern |
| B09d Rev Delay | 30 s | B09d Rev Delay.wav | a click with echoes, then one with reverb |

- **B03** uses all four outputs. With four inputs, save one four-channel file, or two stereo files named `B03 Output Map 1-2.wav` and `B03 Output Map 3-4.wav`. With two inputs, record OUT 1 and 2 only.
- **B08** is a performance (`B08 Clock 120.prf2`): open it the same way.

## 3. The audio input (B10)

1. Load **B10 Audio In**. It sends the G2's IN 1 straight to OUT 1.
2. Connect output 1 of your interface to the G2's **IN 1**. Leave the G2's OUT 1 on input 1 of the interface.
3. If you can, also connect output 2 of your interface directly to its input 2 (unplug the G2's OUT 2 for this). That cable is our reference: it tells us how loud your interface plays the file and how long it takes.
4. Put `B10 Stimulus.wav` on a track that plays on outputs 1 and 2, fader at 0 dB. If the interface has a volume knob for these outputs, turn it all the way up (headphones off): the loudest tones are meant to reach the limit of the G2's input. Switch input monitoring off, or it will howl.
5. Record inputs 1 and 2 while the file plays (14 seconds). Save as `B10 Audio In.wav`.

## 4. A few things to look at

Write the answers in `form.txt` as you go.

1. Switch the G2 off and on. The display shows the OS version for a second (Q04). Before you load anything, press **Patch Settings** if its lamp is off: the small display above knobs 1 and 2 shows a tempo and, under it, Run or Stop (Q15).
2. Load **B11**, **B12** and **B13** in turn. Each asks for 32 voices and is too heavy to get them all. With the Patch Settings lamp on, the display above knob 2 shows something like `03 (32)`: write down exactly what you see (Q12 to Q14). On a G2 Engine, the editor shows the voice count in its toolbar. These three are silent until you play a key.
3. Keyboard models: load **B14 Wheels**. It holds one note. Push the pitch stick to the right: does the note go up or down (Q16)? On a G2X, roll G.Wheel 1 away from you: up or down (Q17)? Roll G.Wheel 2 away from you: does a second, higher note come in or fade out (Q18)?
4. Press **System**, then the Down button until the display says `Memory Prot`, and turn the dial to On. Press **Store** once, and only once. What does the display say (Q19)? If it offers to store, press Patch twice to leave without storing. Then put Memory Protect back as it was.

## 5. Send it back

Fill in the rest of `form.txt`, put it in a folder with your WAV files, zip the folder and send it to the person who gave you this pack. Thank you!

## Optional: try G2fresh with your G2

*Needs G2fresh v0.10.2 or later. On Windows, G2fresh needs the WinUSB driver for the G2 (see G2fresh's README).*

1. Quit Clavia's editor: only one program at a time can talk to the G2.
2. Start G2fresh and choose **Synth > G2 or G2X (USB)**. The status bar shows the G2's version.
3. Choose **Synth > Get Patch from Slot > A**. The patch in slot A should appear.
4. Turn a knob in G2fresh and listen; then turn a knob on the G2 and watch G2fresh follow.
5. Choose **Synth > Send Patch to Slot > B**.
6. Open **Synth > Synth Memory (Banks)...**, look around, and store one patch in an empty location.
7. Slots: choose **Synth > Get Performance from G2**. Buttons **A B C D** appear in G2fresh's toolbar. Click **B**, then **C**. On the G2, does the lit Slot button follow each click, and when you play a key, which slot do you hear (Q21)?
8. **Do this one last, and save your work in the G2 first.** Choose **Patch > Patch Notes...**, type a few words and click OK. Then turn a knob in G2fresh: does the G2 still follow (Q22)? One person reported that this message froze their G2. If yours stops responding, nothing is damaged: switch it off and on, and tell us. Anything you had not stored in the G2 is lost, which is why this step comes last.
9. Whatever happened, choose **Help > Show USB Log** and send us `usb.log` (and `usb.log.1` if there is one) with a few words about what worked and what did not. The log holds the USB traffic only, and leaves the contents of your patches out.
