# asm2memcard
asm2memcard converts GameCube opcodes into a memory card exploit.  Gecko codes, for example.  
It will output an Action Reply code that should be used with Dolphin.

```
Usage:   asm2memcard infile outfile
Example: asm2memcard masterhand.a2m masterhand.txt
```

## Creating a Hacked Memory Card
First, clear the virtual memory card of any SSBM data.  Then run the game with the AR code enabled.
Create a new save file when prompted and hit Start on the title screen.  Wait until the saving icon
in the top right corner of the main menu goes away, then stop Dolphin.  Disable the AR code.  
Congratulations!  You can now use Dolphin's memory card management tool to export a GCI file of your
mod!  Use a homebrew application such as GCMM to get this onto a real memory card.

## Testing Codes
If you are just testing your codes, disable the virtual memory card.
This way you'll be able to use the output AR code to load name entry and test your codes
without the above memory card process.

## Syntax
Comments begin with `;` and end after a newline.  
Directives begin with `@`.  
Addresses and opcodes are all hexadecimal.  Do *NOT* prefix them with `0x`!

### `title` Directive
asm2memcard instructs Melee to create a second save file after loading the exploit to allow normal
usage of nametags.  The title directive allows you to set the name of this second save file.

The given title must be less than 12 characters and is ended by a newline.

### `set` Directive
The same as many simple Action Replay codes.  The first number is the address (offset by 0x80000000) to overwrite, the
second number is the new value.

### `branch` Directive
Almost the same as C2 Gecko codes.  The first number listed is the address (again offset by 0x80000000) of
where the branch occurs.  Any opcodes listed until another directive or the end of input will be
inserted into free memory and executed when the branch occurs.  
A branch back to the original address will be inserted at the end of a branch directive automatically.

### Example a2m File
```
; Hello!

@title Test

; Enable C-Stick in singleplayer.
@set 16B480 60000000

; Branch from 0x80AA694C and crash the game.
@branch AA694C
00000000
```

## Resources
* [wParam originally discovered the nametag exploit](http://wparam.com/ssbm/exploit.html)
* [Smashboards Community Symbol Map](https://smashboards.com/threads/smashboards-community-symbol-map.426763/)
* [Achilles1515's SSBM Facts](https://raw.githubusercontent.com/Achilles1515/20XX-Melee-Hack-Pack/master/SSBM%20Facts.txt)
