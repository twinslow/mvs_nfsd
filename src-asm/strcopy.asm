*---------------------------------------------------------------------*
* Copy a null terminated string to destination and pad with blanks    *
* On entry ...                                                        *
*    R2 - Source address                                              *
*    R3 - Destination address                                         *
*    R4 - Length of destination field                                 *
*    R6 - Return address                                              *
* On exit ...                                                         *
*    R0 - Length of the source string                                 *
*---------------------------------------------------------------------*
STRCOPY  LR    R5,R4          R5 = remaining destination length
         SR    R8,R8          R8 = count of chars copied (return value)
         LTR   R5,R5          destination length zero?
         BZ    STRCEND        yes - nothing to do
*
STRCLP   CLI   0(R2),X'00'    end of source string?
         BE    STRCPAD        yes - go pad remainder
         MVC   0(1,R3),0(R2)  copy one byte
         LA    R2,1(R2)       bump source pointer
         LA    R3,1(R3)       bump destination pointer
         LA    R8,1(R8)       bump count of chars copied
         BCT   R5,STRCLP      decrement remaining, loop if > 0
         B     STRCEND        dest full - truncated, done
*
STRCPAD  LTR   R5,R5          any destination space left?
         BZ    STRCEND        no - done
         MVI   0(R3),C' '     blank fill
         LA    R3,1(R3)       bump destination pointer
         BCT   R5,STRCPAD     decrement remaining, loop if > 0
*
STRCEND  DS    0H
         LR    R0,R8          return copied (real) len in R0
         BR    R6             return to caller (R0 = chars copied)
*