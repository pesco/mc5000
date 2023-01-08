# Turn on an LED. Flash it when the chip (re)boots or a button is pressed.
# 
# p0 button
# p1 LED

 mov 0 p1
 slp 1
 mov 100 p1
 slp 1
 mov 0 p1
 slp 10
on:
 mov 100 p1
 teq p0 100
-jmp on
