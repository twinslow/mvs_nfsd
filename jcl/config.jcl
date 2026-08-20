[Init]

set loglvl info
set wtolvl info

[Exports]

# Export some PDS datasets
/exports      TEMP.TESTPROJ.C                    dirperm=777 memperm=666
/exports      TEMP.TESTPROJ.CNTL     fileext=jcl dirperm=777 memperm=777
/exports      TEMP.TESTPROJ.JCLLIB   fileext=jcl dirperm=777 memperm=666
/exports      SYS1.SAMPLIB           nofileext   ro
/exports      TONYW.SOCKTEST.C       fileext=c
/exports      TONYW.SOCKTEST.JCL     fileext=jcl

# Alternate format for exports
/exp2 dirperm=777 memperm=666 {
    SOME.TESTPROJ.C
    SOME.TESTPROJ.CNTL     fileext=jcl memperm=777
    SOME.TESTPROJ.JCLLIB   fileext=jcl
    SYS2.PROCLIB                       ro
    SYS2.JCLLIB            fileext=jcl ro
    SYS1.SAMPLIB           nofileext   ro
}
