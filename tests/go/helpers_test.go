package adbcbridge_test

import (
	"github.com/apache/arrow-go/v18/arrow"
)

type arrow_Array = arrow.Array

func arrowSchema(name string) *arrow.Schema {
	return arrow.NewSchema([]arrow.Field{{Name: name, Type: arrow.PrimitiveTypes.Int64, Nullable: true}}, nil)
}
