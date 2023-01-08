# p0 button
# p1 LED

start:
 teq p0 100
+mov 100 p1
 nop
+slp 5
+mov 0 p1
 slp 5

-nop #keck wirst

 jmp end
 end:jmp start

