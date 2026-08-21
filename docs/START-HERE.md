# Start here

The simple version. Six steps. Do them in order.

You need: the board, a USB-C cable that came with a phone or a hard drive (a charging-only cable will
not work), and this computer.

---

## Step 1 - Install the driver

Windows cannot talk to the board until you install one small driver.

1. Go to **https://sparks.gogo.co.nz/ch340.html**
2. Download the Windows driver from that page.
3. Run the file you downloaded. Click **Install**. Wait for "Driver install success".
4. Close it.

You only ever do this once on this computer.

---

## Step 2 - Plug in the board

Plug the board into a USB port on the computer.

**What you should see:** the little screen may show random dots, or nothing at all. Either is fine.
The board has no program on it yet.

**If nothing at all happens** (no light anywhere on the board): your cable is a charging-only cable.
Try a different one. This is the single most common problem.

---

## Step 3 - Open PowerShell in the project folder

1. Open the folder holding these files in File Explorer.
2. Hold **Shift** and **right-click** on an empty part of that window.
3. Choose **Open PowerShell window here** (some versions say **Open Terminal**).

A black window opens with white text, already pointing at the right folder.

**Tip:** to paste into this window, **right-click**. Ctrl+V may not work.

---

## Step 4 - Copy this and paste it into the black window

Copy this whole block.

```
$f = [string][char]0x60 * 3
$d = if (Test-Path .\docs\INSTALLER.md) { '.\docs' } else { '.' }
$m = Get-Content "$d\INSTALLER.md" -Raw
$p = "(?ms)^${f}powershell\r?\n(.*?)^${f}"
$s = [regex]::Match($m, $p).Groups[1].Value
$e = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText("$pwd\install.ps1", $s, $e)
$s.Length
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

Right-click in the black window to paste, then press **Enter**.

The line `$s.Length` prints a number. It should be around **9000**. If it prints **0**, stop and say
so - nothing else will work.

The installer puts an **Install flock-mini** icon on your Desktop. From now on you can just
double-click that icon instead of typing anything.

**Never double-click the `.ps1` file itself.** Windows does not know how to run those and will ask
you "how do you want to open this file". Use the Desktop icon.

Now wait. The first time takes about ten minutes, because it downloads a large toolkit. You will see
a lot of text scroll past. That is normal and good.

---

## Step 5 - Answer the two questions

The installer stops and asks you things. Both times, just press **Enter** to say yes.

1. `Flash the board on COM5 now? [Y/n]` - press **Enter**
2. `Open the serial monitor? [Y/n]` - press **Enter**

(Your number might be COM4 or COM7 instead of COM5. That is fine.)

---

## Step 6 - Look at the board

**On the little screen:** first the word `FLOCK-MINI`, then a screen that says `Scanning...` with a
number in the top right corner that keeps changing between 1, 6, and 11.

**In the black window:** a line like this, repeating every 30 seconds:

```
[flock-mini] ch=6 seen=0 high=0 drops=0 heap=41800
```

That is it. It works. The device is now listening for Flock cameras.

Press **Ctrl+C** in the black window when you want to stop watching. The board keeps running on its
own, and it will run off any USB power bank.

---

# What the screen is telling you

- **`Scanning...`** - normal. Nothing found nearby.
- **`FLOCK DETECTED`** - it found something. The screen shows the device's ID number, how strong the
  signal is, and CLOSE, NEAR, or FAR.
- **The list screen** - everything found so far. Rows marked **HI** are probably real cameras. Rows
  marked **lo** are weak guesses that are often ordinary smart-home gadgets, so do not trust those.

The button on the board: **tap** it to flip through the list, **hold** it for one second to silence
alerts.

---

# Something went wrong

**"cannot be loaded because running scripts is disabled"**
Close the window, open PowerShell again, and paste this first:
`Set-ExecutionPolicy -Scope Process Bypass -Force`
Then do Step 4 again.

**It said "No USB serial adapter detected"**
The board is not plugged in, the driver did not install, or the cable is charging-only. Go back to
Steps 1 and 2. You can re-run everything by typing `.\install.ps1` and pressing Enter.

**It stopped and said "Upload failed"**
Do this on the board: hold the **FLASH** button down, tap the **RST** button, let go of **FLASH**.
Then type `.\install.ps1` and press Enter to try again.

**The board keeps restarting itself**
It is not getting enough power. Plug it into a USB port on the back of the computer, or use a
different port. Some front ports and some hubs are too weak.

**The screen stays completely dark but the text in the window looks fine**
Look for a line saying `no I2C device at 0x3C`. If it is there, tell me and I will adjust one setting.

**Anything else**
Copy the last twenty lines of text from the black window and show them to me.

---

# Testing it without a real camera

You probably have no camera nearby, so here is how to prove it actually works. Your phone sends out
the same kind of radio signal a camera does when it looks for WiFi.

1. On your phone, find its WiFi address:
   - Android: **Settings > About phone > Status > WiFi MAC address**
   - iPhone: **Settings > General > About > Wi-Fi Address**
2. It looks like `a1:b2:c3:d4:e5:f6`. You only need the **first three pairs**: `a1b2c3`.
3. Tell me those three pairs and I will give you a one-line change.
4. Turn your phone's WiFi off and back on. The board should shout `FLOCK DETECTED`.

That proves the whole thing works. Then we undo the change so your phone stops setting it off.

---

# A promise about what this does

It only listens. It never sends anything out, never interferes with anything, never connects to any
network. It is a radio scanner, like listening to a police scanner. It cannot see Bluetooth at all,
because this particular board has no Bluetooth chip in it.
