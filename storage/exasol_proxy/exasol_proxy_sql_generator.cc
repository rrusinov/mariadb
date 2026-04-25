#ifndef MYSQL_SERVER
#define MYSQL_SERVER 1
#endif

#include "exasol_proxy_sql_generator.h"

#include <my_global.h>

#include "item.h"
#include "item_func.h"
#include "item_sum.h"
#include "m_string.h"
#include "my_decimal.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "table.h"

#include <cctype>
#include <cstring>
#include <string>

namespace exasol_proxy
{
namespace
{

bool is_empty(const LEX_CSTRING &value)
{
  return !value.str || value.length == 0;
}

std::string string_from(const LEX_CSTRING &value)
{
  return value.str ? std::string(value.str, value.length) : std::string();
}

bool equals_ignore_case(const LEX_CSTRING &value, const char *literal)
{
  if (!value.str || !literal)
    return false;
  const size_t literal_length= std::strlen(literal);
  if (value.length != literal_length)
    return false;
  for (size_t i= 0; i < literal_length; ++i)
  {
    const auto left= static_cast<unsigned char>(value.str[i]);
    const auto right= static_cast<unsigned char>(literal[i]);
    if (std::tolower(left) != std::tolower(right))
      return false;
  }
  return true;
}

std::string quote_identifier(const LEX_CSTRING &identifier)
{
  std::string quoted{"\""};
  if (identifier.str)
  {
    for (size_t i= 0; i < identifier.length; ++i)
    {
      if (identifier.str[i] == '"')
        quoted+= "\"\"";
      else
        quoted+= identifier.str[i];
    }
  }
  quoted+= '"';
  return quoted;
}

std::string quote_string(const String &value)
{
  std::string quoted{"'"};
  for (size_t i= 0; i < value.length(); ++i)
  {
    if (value.ptr()[i] == '\'')
      quoted+= "''";
    else
      quoted+= value.ptr()[i];
  }
  quoted+= "'";
  return quoted;
}

std::string integer_to_string(longlong value)
{
  char buffer[64];
  my_snprintf(buffer, sizeof(buffer), "%lld", value);
  return buffer;
}

std::string double_to_string(double value)
{
  char buffer[FLOATING_POINT_BUFFER];
  const size_t length= my_gcvt(value, MY_GCVT_ARG_DOUBLE, sizeof(buffer) - 1, buffer, nullptr);
  return std::string(buffer, length);
}

SqlGenerationResult decimal_to_sql(const my_decimal &value)
{
  StringBuffer<64> buffer;
  if (!value.to_string(&buffer))
    return SqlGenerationResult::unsupported("failed to render decimal constant");
  return SqlGenerationResult::generated(std::string(buffer.ptr(), buffer.length()));
}

class Generator
{
public:
  explicit Generator(THD *thd_arg) : thd(thd_arg) {}

  SqlGenerationResult generate_order_sql(ORDER *order)
  {
    if (!thd)
      return unsupported("missing MariaDB THD");
    return emit_order_list(order, true);
  }

  SqlGenerationResult generate(st_select_lex_unit *lex_unit)
  {
    if (!thd)
      return unsupported("missing MariaDB THD");
    if (!lex_unit)
      return unsupported("missing SELECT_LEX_UNIT");

    st_select_lex *first= lex_unit->first_select();
    if (!first)
      return unsupported("empty SELECT_LEX_UNIT");
    if (first->next_select())
      return generate_compound_select(lex_unit);

    return generate(first);
  }

  SqlGenerationResult generate(st_select_lex *sel_lex)
  {
    if (!thd)
      return unsupported("missing MariaDB THD");
    if (!sel_lex)
      return unsupported("missing SELECT_LEX");

    std::string sql= "SELECT ";
    if (sel_lex->options & SELECT_DISTINCT)
      sql+= "DISTINCT ";

    auto select_list= emit_select_list(sel_lex->item_list);
    if (!select_list.supported())
      return select_list;
    sql+= select_list.sql;

    if (sel_lex->table_list.elements)
    {
      auto from= emit_table_list(sel_lex->table_list);
      if (!from.supported())
        return from;
      sql+= " FROM ";
      sql+= from.sql;
    }

    if (sel_lex->where)
    {
      auto where= emit_expression(sel_lex->where);
      if (!where.supported())
        return where;
      sql+= " WHERE ";
      sql+= where.sql;
    }

    if (sel_lex->group_list.elements)
    {
      auto group_by= emit_order_list(sel_lex->group_list.first, false);
      if (!group_by.supported())
        return group_by;
      sql+= " GROUP BY ";
      sql+= group_by.sql;
    }

    if (sel_lex->having)
    {
      auto having= emit_expression(sel_lex->having);
      if (!having.supported())
        return having;
      sql+= " HAVING ";
      sql+= having.sql;
    }

    if (sel_lex->order_list.elements)
    {
      auto order_by= emit_order_list(sel_lex->order_list.first, true);
      if (!order_by.supported())
        return order_by;
      sql+= " ORDER BY ";
      sql+= order_by.sql;
    }

    if (sel_lex->limit_params.with_ties)
      return unsupported("LIMIT WITH TIES emission is not supported");

    if (sel_lex->limit_params.explicit_limit && sel_lex->limit_params.select_limit)
    {
      auto limit= emit_integer_constant(sel_lex->limit_params.select_limit);
      if (!limit.supported())
        return limit;
      sql+= " LIMIT ";
      sql+= limit.sql;
    }

    if (sel_lex->limit_params.explicit_limit && sel_lex->limit_params.offset_limit)
    {
      auto offset= emit_integer_constant(sel_lex->limit_params.offset_limit);
      if (!offset.supported())
        return offset;
      sql+= " OFFSET ";
      sql+= offset.sql;
    }

    return SqlGenerationResult::generated(std::move(sql));
  }

private:
  THD *thd;

  SqlGenerationResult generate_compound_select(st_select_lex_unit *lex_unit)
  {
    std::string sql;
    bool first_select= true;
    for (st_select_lex *select= lex_unit->first_select(); select; select= select->next_select())
    {
      auto select_sql= generate(select);
      if (!select_sql.supported())
        return select_sql;

      if (!first_select)
      {
        auto operation= emit_set_operation(select);
        if (!operation.supported())
          return operation;
        sql+= " ";
        sql+= operation.sql;
        sql+= " ";
      }

      sql+= "(";
      sql+= select_sql.sql;
      sql+= ")";
      first_select= false;
    }

    auto global_order_limit= emit_global_order_limit(lex_unit);
    if (!global_order_limit.supported())
      return global_order_limit;
    sql+= global_order_limit.sql;

    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_set_operation(st_select_lex *select)
  {
    std::string operation;
    switch (select->get_linkage())
    {
      case UNION_TYPE:
        operation= "UNION";
        break;
      case INTERSECT_TYPE:
        operation= "INTERSECT";
        break;
      case EXCEPT_TYPE:
        operation= "EXCEPT";
        break;
      default:
        return unsupported("unsupported compound SELECT linkage");
    }

    if (select->with_all_modifier)
      operation+= " ALL";

    return SqlGenerationResult::generated(std::move(operation));
  }

  SqlGenerationResult emit_select_list(List<Item> &items)
  {
    if (items.is_empty())
      return unsupported("empty SELECT item list");

    std::string sql;
    bool first= true;
    for (Item &item_ref : items)
    {
      Item *item= &item_ref;
      auto expression= emit_expression(item);
      if (!expression.supported())
        return expression;

      if (!first)
        sql+= ", ";
      first= false;
      sql+= expression.sql;

      if (should_emit_alias(item))
      {
        sql+= " AS ";
        sql+= quote_identifier(item->name);
      }
    }
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_table_list(SQL_I_List<TABLE_LIST> &tables)
  {
    std::string sql;
    bool first= true;
    for (TABLE_LIST *table= tables.first; table; table= table->next_local)
    {
      if (table->nested_join)
        return unsupported("nested join emission is not implemented yet");
      if (table->table_function)
        return unsupported("table function emission is not supported");
      if (table->natural_join || table->join_using_fields)
        return unsupported("NATURAL/USING join emission is not implemented yet");

      auto table_ref= emit_table_ref(table);
      if (!table_ref.supported())
        return table_ref;

      if (!first)
      {
        if (table->on_expr || table->outer_join)
        {
          auto join_keyword= emit_join_keyword(table);
          if (!join_keyword.supported())
            return join_keyword;
          sql+= " ";
          sql+= join_keyword.sql;
          sql+= " ";
          sql+= table_ref.sql;
          if (table->on_expr)
          {
            auto condition= emit_expression(table->on_expr);
            if (!condition.supported())
              return condition;
            sql+= " ON ";
            sql+= condition.sql;
          }
          else
            return unsupported("outer join without ON expression is not supported");
          first= false;
          continue;
        }
        else
          sql+= ", ";
      }
      first= false;
      sql+= table_ref.sql;
    }
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_table_ref(TABLE_LIST *table)
  {
    std::string sql;

    if (table->derived && table->is_anonymous_derived_table())
    {
      auto derived= generate(table->derived);
      if (!derived.supported())
        return derived;
      sql+= "(";
      sql+= derived.sql;
      sql+= ")";
      if (is_empty(table->alias))
        return unsupported("derived table has no alias");
      sql+= " AS ";
      sql+= quote_identifier(table->alias);
      return SqlGenerationResult::generated(std::move(sql));
    }

    if (table->derived)
      return unsupported("view-backed derived table emission is not implemented yet");

    if (!is_empty(table->db))
    {
      sql+= quote_identifier(table->db);
      sql+= ".";
    }
    sql+= quote_identifier(table->get_table_name());

    if (!is_empty(table->alias) &&
        string_from(table->alias) != string_from(table->get_table_name()))
    {
      sql+= " AS ";
      sql+= quote_identifier(table->alias);
    }

    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_join_keyword(TABLE_LIST *table)
  {
    std::string sql;
    if (table->outer_join)
    {
      if (table->outer_join & JOIN_TYPE_LEFT)
        sql+= "LEFT JOIN";
      else if (table->outer_join & JOIN_TYPE_RIGHT)
        sql+= "RIGHT JOIN";
      else
        return unsupported("unknown outer join type");
    }
    else
      sql+= "JOIN";

    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_global_order_limit(st_select_lex_unit *lex_unit)
  {
    st_select_lex *parameters= lex_unit->global_parameters();
    if (!parameters)
      return SqlGenerationResult::generated(std::string());

    std::string sql;
    if (parameters->order_list.elements)
    {
      auto order_by= emit_order_list(parameters->order_list.first, true);
      if (!order_by.supported())
        return order_by;
      sql+= " ORDER BY ";
      sql+= order_by.sql;
    }

    if (parameters->limit_params.with_ties)
      return unsupported("compound SELECT LIMIT WITH TIES emission is not supported");

    if (parameters->limit_params.explicit_limit && parameters->limit_params.select_limit)
    {
      auto limit= emit_integer_constant(parameters->limit_params.select_limit);
      if (!limit.supported())
        return limit;
      sql+= " LIMIT ";
      sql+= limit.sql;
    }

    if (parameters->limit_params.explicit_limit && parameters->limit_params.offset_limit)
    {
      auto offset= emit_integer_constant(parameters->limit_params.offset_limit);
      if (!offset.supported())
        return offset;
      sql+= " OFFSET ";
      sql+= offset.sql;
    }

    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_order_list(ORDER *order, bool include_direction)
  {
    std::string sql;
    for (; order; order= order->next)
    {
      if (!sql.empty())
        sql+= ", ";

      if (order->counter_used)
        sql+= integer_to_string(order->counter);
      else if (order->item && order->item[0] && order->item[0]->is_order_clause_position())
        sql+= integer_to_string((*order->item)->val_int());
      else if (order->item && *order->item)
      {
        auto expression= emit_expression(*order->item);
        if (!expression.supported())
          return expression;
        sql+= expression.sql;
      }
      else
        return unsupported("ORDER/GROUP item is missing");

      if (include_direction)
      {
        if (order->direction == ORDER::ORDER_DESC)
          sql+= " DESC";
        else
          sql+= " ASC";
      }
    }
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_expression(Item *item)
  {
    if (!item)
      return unsupported("missing expression");

    switch (item->type())
    {
      case Item::FIELD_ITEM:
        return emit_identifier(static_cast<Item_ident *>(item));
      case Item::REF_ITEM:
        return emit_ref(static_cast<Item_ref *>(item));
      case Item::CONST_ITEM:
        return emit_constant(item);
      case Item::NULL_ITEM:
        return SqlGenerationResult::generated("NULL");
      case Item::FUNC_ITEM:
        return emit_function(static_cast<Item_func *>(item));
      case Item::SUM_FUNC_ITEM:
        return emit_aggregate(static_cast<Item_sum *>(item));
      default:
        return unsupported("unsupported MariaDB Item type");
    }
  }

  SqlGenerationResult emit_ref(Item_ref *item)
  {
    if (item->ref && *item->ref)
      return emit_expression(*item->ref);
    return emit_identifier(item);
  }

  SqlGenerationResult emit_identifier(Item_ident *item)
  {
    std::string sql;
    if (!is_empty(item->db_name))
    {
      sql+= quote_identifier(item->db_name);
      sql+= ".";
    }
    if (!is_empty(item->table_name))
    {
      sql+= quote_identifier(item->table_name);
      sql+= ".";
    }
    if (is_empty(item->field_name))
      return unsupported("field identifier has no field name");
    sql+= quote_identifier(item->field_name);
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_constant(Item *item)
  {
    const Item_const *constant= item->get_item_const();
    if (!constant)
      return unsupported("unsupported constant expression");
    if (constant->const_is_null())
      return SqlGenerationResult::generated("NULL");
    if (const longlong *value= constant->const_ptr_longlong())
      return SqlGenerationResult::generated(integer_to_string(*value));
    if (const my_decimal *value= constant->const_ptr_my_decimal())
      return decimal_to_sql(*value);
    if (const double *value= constant->const_ptr_double())
      return SqlGenerationResult::generated(double_to_string(*value));
    if (const String *value= constant->const_ptr_string())
      return SqlGenerationResult::generated(quote_string(*value));
    return unsupported("unsupported constant expression");
  }

  SqlGenerationResult emit_integer_constant(Item *item)
  {
    if (!item)
      return unsupported("missing integer constant");
    const Item_const *constant= item->get_item_const();
    if (constant)
    {
      if (const longlong *value= constant->const_ptr_longlong())
        return SqlGenerationResult::generated(integer_to_string(*value));
    }
    if (item->const_item() && item->result_type() == INT_RESULT)
      return SqlGenerationResult::generated(integer_to_string(item->val_int()));
    return unsupported("non-integer LIMIT/OFFSET expression is not supported");
  }

  SqlGenerationResult emit_function(Item_func *function)
  {
    switch (function->functype())
    {
      case Item_func::EQ_FUNC:
        return emit_binary_function(function, "=");
      case Item_func::NE_FUNC:
        return emit_binary_function(function, "<>");
      case Item_func::LT_FUNC:
        return emit_binary_function(function, "<");
      case Item_func::LE_FUNC:
        return emit_binary_function(function, "<=");
      case Item_func::GE_FUNC:
        return emit_binary_function(function, ">=");
      case Item_func::GT_FUNC:
        return emit_binary_function(function, ">");
      case Item_func::LIKE_FUNC:
        return emit_binary_function(function, "LIKE");
      case Item_func::ISNULL_FUNC:
        return emit_unary_suffix_function(function, "IS NULL");
      case Item_func::ISNOTNULL_FUNC:
        return emit_unary_suffix_function(function, "IS NOT NULL");
      case Item_func::COND_AND_FUNC:
        return emit_variadic_infix_function(function, "AND");
      case Item_func::COND_OR_FUNC:
        return emit_variadic_infix_function(function, "OR");
      case Item_func::NOT_FUNC:
        return emit_unary_prefix_function(function, "NOT");
      case Item_func::BETWEEN:
        return emit_between_function(function);
      case Item_func::IN_FUNC:
        return emit_in_function(function);
      case Item_func::NEG_FUNC:
        return emit_unary_prefix_function(function, "-");
      case Item_func::YEAR_FUNC:
        return emit_extract_function(function, "YEAR");
      case Item_func::EQUAL_FUNC:
        return unsupported("NULL-safe equality emission is not implemented yet");
      default:
        return emit_named_or_operator_function(function);
    }
  }

  SqlGenerationResult emit_named_or_operator_function(Item_func *function)
  {
    const LEX_CSTRING name= function->func_name_cstring();
    if (equals_ignore_case(name, "ifnull"))
      return emit_named_function(function, "IFNULL");
    if (equals_ignore_case(name, "coalesce"))
      return emit_named_function(function, "COALESCE");
    if (equals_ignore_case(name, "concat"))
      return emit_named_function(function, "CONCAT");
    if (equals_ignore_case(name, "abs"))
      return emit_named_function(function, "ABS");
    if (equals_ignore_case(name, "lower") || equals_ignore_case(name, "lcase"))
      return emit_named_function(function, "LOWER");
    if (equals_ignore_case(name, "upper") || equals_ignore_case(name, "ucase"))
      return emit_named_function(function, "UPPER");
    if (equals_ignore_case(name, "left"))
      return emit_named_function(function, "LEFT");
    if (equals_ignore_case(name, "substr") || equals_ignore_case(name, "substring"))
      return emit_named_function(function, "SUBSTR");
    if (equals_ignore_case(name, "mod"))
      return emit_named_function(function, "MOD");

    if (name.length == 1)
    {
      switch (name.str[0])
      {
        case '+':
          return emit_binary_function(function, "+");
        case '-':
          return emit_binary_function(function, "-");
        case '*':
          return emit_binary_function(function, "*");
        case '/':
          return emit_binary_function(function, "/");
        default:
          break;
      }
    }
    return unsupported("unsupported scalar function");
  }

  SqlGenerationResult emit_named_function(Item_func *function, const char *function_name)
  {
    if (function->argument_count() == 0)
      return unsupported("named function has no arguments");

    std::string sql= std::string(function_name) + "(";
    for (uint i= 0; i < function->argument_count(); ++i)
    {
      auto expression= emit_expression(function->arguments()[i]);
      if (!expression.supported())
        return expression;
      if (i > 0)
        sql+= ", ";
      sql+= expression.sql;
    }
    sql+= ")";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_extract_function(Item_func *function, const char *field_name)
  {
    if (function->argument_count() != 1)
      return unsupported("EXTRACT function has unexpected argument count");
    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;
    return SqlGenerationResult::generated("EXTRACT(" + std::string(field_name) + " FROM " +
                                          expression.sql + ")");
  }

  SqlGenerationResult emit_binary_function(Item_func *function, const char *operator_text)
  {
    if (function->argument_count() != 2)
      return unsupported("binary function has unexpected argument count");
    auto left= emit_expression(function->arguments()[0]);
    if (!left.supported())
      return left;
    auto right= emit_expression(function->arguments()[1]);
    if (!right.supported())
      return right;

    return SqlGenerationResult::generated("(" + left.sql + " " + operator_text + " " +
                                          right.sql + ")");
  }

  SqlGenerationResult emit_unary_suffix_function(Item_func *function, const char *suffix)
  {
    if (function->argument_count() != 1)
      return unsupported("unary suffix function has unexpected argument count");
    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;
    return SqlGenerationResult::generated("(" + expression.sql + " " + suffix + ")");
  }

  SqlGenerationResult emit_unary_prefix_function(Item_func *function, const char *prefix)
  {
    if (function->argument_count() != 1)
      return unsupported("unary prefix function has unexpected argument count");
    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;
    return SqlGenerationResult::generated("(" + std::string(prefix) + " " + expression.sql + ")");
  }

  SqlGenerationResult emit_variadic_infix_function(Item_func *function, const char *operator_text)
  {
    if (function->argument_count() == 0)
      return unsupported("variadic function has no arguments");

    std::string sql= "(";
    for (uint i= 0; i < function->argument_count(); ++i)
    {
      auto expression= emit_expression(function->arguments()[i]);
      if (!expression.supported())
        return expression;
      if (i > 0)
      {
        sql+= " ";
        sql+= operator_text;
        sql+= " ";
      }
      sql+= expression.sql;
    }
    sql+= ")";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_between_function(Item_func *function)
  {
    if (function->argument_count() != 3)
      return unsupported("BETWEEN function has unexpected argument count");
    auto value= emit_expression(function->arguments()[0]);
    if (!value.supported())
      return value;
    auto lower= emit_expression(function->arguments()[1]);
    if (!lower.supported())
      return lower;
    auto upper= emit_expression(function->arguments()[2]);
    if (!upper.supported())
      return upper;
    return SqlGenerationResult::generated("(" + value.sql + " BETWEEN " + lower.sql + " AND " +
                                          upper.sql + ")");
  }

  SqlGenerationResult emit_in_function(Item_func *function)
  {
    if (function->argument_count() < 2)
      return unsupported("IN function has unexpected argument count");
    auto value= emit_expression(function->arguments()[0]);
    if (!value.supported())
      return value;

    std::string sql= "(" + value.sql + " IN (";
    for (uint i= 1; i < function->argument_count(); ++i)
    {
      auto expression= emit_expression(function->arguments()[i]);
      if (!expression.supported())
        return expression;
      if (i > 1)
        sql+= ", ";
      sql+= expression.sql;
    }
    sql+= "))";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_aggregate(Item_sum *aggregate)
  {
    const char *function_name= nullptr;
    bool distinct= false;

    switch (aggregate->sum_func())
    {
      case Item_sum::COUNT_FUNC:
        function_name= "COUNT";
        break;
      case Item_sum::COUNT_DISTINCT_FUNC:
        function_name= "COUNT";
        distinct= true;
        break;
      case Item_sum::SUM_FUNC:
        function_name= "SUM";
        break;
      case Item_sum::SUM_DISTINCT_FUNC:
        function_name= "SUM";
        distinct= true;
        break;
      case Item_sum::AVG_FUNC:
        function_name= "AVG";
        break;
      case Item_sum::AVG_DISTINCT_FUNC:
        function_name= "AVG";
        distinct= true;
        break;
      case Item_sum::MIN_FUNC:
        function_name= "MIN";
        break;
      case Item_sum::MAX_FUNC:
        function_name= "MAX";
        break;
      default:
        return unsupported("unsupported aggregate function");
    }

    std::string sql= std::string(function_name) + "(";
    if (distinct)
      sql+= "DISTINCT ";

    if (aggregate->argument_count() == 0)
      sql+= "*";
    else
    {
      for (uint i= 0; i < aggregate->argument_count(); ++i)
      {
        auto expression= emit_expression(aggregate->arguments()[i]);
        if (!expression.supported())
          return expression;
        if (i > 0)
          sql+= ", ";
        sql+= expression.sql;
      }
    }

    sql+= ")";
    return SqlGenerationResult::generated(std::move(sql));
  }

  bool should_emit_alias(Item *item)
  {
    if (!item || is_empty(item->name))
      return false;
    if (item->type() != Item::FIELD_ITEM)
      return true;

    Item_ident *ident= static_cast<Item_ident *>(item);
    return string_from(item->name) != string_from(ident->field_name);
  }

  static SqlGenerationResult unsupported(const char *reason)
  {
    return SqlGenerationResult::unsupported(reason);
  }
};

} // namespace

SqlGenerationResult generate_exasol_sql(THD *thd, st_select_lex_unit *lex_unit)
{
  return Generator(thd).generate(lex_unit);
}

SqlGenerationResult generate_exasol_sql(THD *thd, st_select_lex *sel_lex)
{
  return Generator(thd).generate(sel_lex);
}

SqlGenerationResult generate_exasol_order_sql(THD *thd, st_order *order)
{
  return Generator(thd).generate_order_sql(order);
}

} // namespace exasol_proxy
