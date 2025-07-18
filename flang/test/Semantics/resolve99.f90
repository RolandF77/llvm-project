! RUN: %python %S/test_errors.py %s %flang_fc1 -pedantic
! Tests for the index-name of a FORALL statement

module m1
  integer modVar
end module m1

program indexName
  common /iCommonName/ x
  type ::  typeName
  end type
  iGlobalVar = 216

contains
  subroutine hostAssoc()
    integer, dimension(4) :: table

  ! iGlobalVar is host associated with the global variable
    iGlobalVar = 1
    FORALL (iGlobalVar=1:4) table(iGlobalVar) = 343
  end subroutine hostAssoc

  subroutine useAssoc()
    use m1
    integer, dimension(4) :: tab
  ! modVar is use associated with the module variable
    FORALL (modVar=1:4) tab(modVar) = 343
  end subroutine useAssoc

  subroutine constructAssoc()
    integer, dimension(4) :: table
    integer :: localVar
    associate (assocVar => localVar)
      !PORTABILITY: Index variable 'assocvar' should be a scalar object or common block if it is present in the enclosing scope [-Wodd-index-variable-restrictions]
      FORALL (assocVar=1:4) table(assocVar) = 343
    end associate
  end subroutine constructAssoc

  subroutine commonSub()
    integer, dimension(4) :: tab
    ! This reference is OK
    FORALL (iCommonName=1:4) tab(iCommonName) = 343
  end subroutine commonSub

  subroutine mismatch()
    integer, dimension(4) :: table
    !PORTABILITY: Index variable 'typename' should be a scalar object or common block if it is present in the enclosing scope [-Wodd-index-variable-restrictions]
    !ERROR: Must have INTEGER type, but is REAL(4)
    !ERROR: Must have INTEGER type, but is REAL(4)
    FORALL (typeName=1:4) table(typeName) = 343
  end subroutine mismatch
end program indexName
