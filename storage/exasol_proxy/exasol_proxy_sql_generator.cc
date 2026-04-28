#ifndef MYSQL_SERVER
#define MYSQL_SERVER 1
#endif

#include "exasol_proxy_sql_generator.h"

#include <my_global.h>

#include "item.h"
#include "item_cmpfunc.h"
#include "item_func.h"
#include "item_sum.h"
#include "item_subselect.h"
#include "item_timefunc.h"
#include "item_windowfunc.h"
#include "m_string.h"
#include "my_decimal.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_window.h"
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

SqlGenerationResult temporal_to_sql(const MYSQL_TIME &value, uint decimals)
{
  char buffer[MAX_DATE_STRING_REP_LENGTH];
  std::string sql;
  switch (value.time_type)
  {
    case MYSQL_TIMESTAMP_DATE:
    {
      const int length= my_date_to_str(&value, buffer);
      sql= "DATE '";
      sql.append(buffer, length);
      sql+= "'";
      return SqlGenerationResult::generated(std::move(sql));
    }
    case MYSQL_TIMESTAMP_DATETIME:
    {
      const int length= my_datetime_to_str(&value, buffer, decimals);
      sql= "TIMESTAMP '";
      sql.append(buffer, length);
      sql+= "'";
      return SqlGenerationResult::generated(std::move(sql));
    }
    case MYSQL_TIMESTAMP_TIME:
    {
      const int length= my_time_to_str(&value, buffer, decimals);
      sql= "TIME '";
      sql.append(buffer, length);
      sql+= "'";
      return SqlGenerationResult::generated(std::move(sql));
    }
    default:
      return SqlGenerationResult::unsupported("unsupported temporal constant");
  }
}

class Generator
{
public:
  explicit Generator(THD *thd_arg) : thd(thd_arg) {}

  static void register_transforms()
  {
    // Function transforms (mirrors SQLGlot TRANSFORMS pattern)
    // TODO: Populate function_transforms with MariaDB Item_func::functype → translator
    // TODO: Populate aggregate_transforms with MariaDB aggregate type → translator
    // TODO: Populate window_transforms with MariaDB window type → translator
    
    // Example pattern:
    // function_transforms[Item_func::BETWEEN] = [](Item_func *f) { return emit_between_function(f); };
    // function_transforms[Item_func::LIKE] = [](Item_func *f) { return emit_like_function(f); };
  }

  SqlGenerationResult generate_order_sql(ORDER *order)
  {
    if (!thd)
      return unsupported("missing MariaDB THD");
    return emit_order_list(order, true);
  }

  SqlGenerationResult generate(st_select_lex_unit *lex_unit)
  {
    return generate_unit(lex_unit, false);
  }

  SqlGenerationResult generate(st_select_lex *sel_lex)
  {
    return generate_select(sel_lex, false);
  }

private:
  THD *thd;

  // SQLGlot-style TRANSFORMS dispatch tables
  // Maps MariaDB Item types → Translator lambdas
  static inline std::map<int, FuncTransform> function_transforms;
  static inline std::map<int, AggregateTransform> aggregate_transforms;
  static inline std::map<int, WindowTransform> window_transforms;
  static inline std::map<int, SubqueryTransform> subquery_transforms;

  SqlGenerationResult dispatch_function(Item_func *function);

  SqlGenerationResult generate_unit(st_select_lex_unit *lex_unit, bool suppress_positive_limit)
  {
    if (!thd)
      return unsupported("missing MariaDB THD");
    if (!lex_unit)
      return unsupported("missing SELECT_LEX_UNIT");

    st_select_lex *first= lex_unit->first_select();
    if (!first)
      return unsupported("empty SELECT_LEX_UNIT");
    if (first->next_select())
      return generate_compound_select(lex_unit, suppress_positive_limit);

    return generate_select(first, suppress_positive_limit);
  }

  SqlGenerationResult generate_select(st_select_lex *sel_lex, bool suppress_positive_limit)
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
      auto olap_prefix= emit_olap_group_prefix(sel_lex);
      if (!olap_prefix.supported())
        return olap_prefix;
      sql+= " GROUP BY ";
      sql+= olap_prefix.sql;
      sql+= group_by.sql;
      if (!olap_prefix.sql.empty())
        sql+= ")";
    }
    else if (sel_lex->olap != UNSPECIFIED_OLAP_TYPE)
      return unsupported("OLAP grouping without GROUP BY is not supported");

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

    const bool suppress_limit= should_suppress_positive_limit(sel_lex, suppress_positive_limit);
    if (!suppress_limit && sel_lex->limit_params.explicit_limit &&
        sel_lex->limit_params.select_limit)
    {
      auto limit= emit_integer_constant(sel_lex->limit_params.select_limit);
      if (!limit.supported())
        return limit;
      sql+= " LIMIT ";
      sql+= limit.sql;
    }

    if (!suppress_limit && sel_lex->limit_params.explicit_limit &&
        sel_lex->limit_params.offset_limit)
    {
      auto offset= emit_integer_constant(sel_lex->limit_params.offset_limit);
      if (!offset.supported())
        return offset;
      sql+= " OFFSET ";
      sql+= offset.sql;
    }

    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult generate_compound_select(st_select_lex_unit *lex_unit,
                                               bool suppress_positive_limit)
  {
    std::string sql;
    bool first_select= true;
    for (st_select_lex *select= lex_unit->first_select(); select; select= select->next_select())
    {
      auto select_sql= generate_select(select, false);
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

    auto global_order_limit= emit_global_order_limit(lex_unit, suppress_positive_limit);
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
    if (tables.first && (tables.first->outer_join & JOIN_TYPE_RIGHT))
    {
      if (!tables.first->next_local || tables.first->next_local->next_local)
        return unsupported("RIGHT JOIN emission is only implemented for simple two-table joins");
      return emit_simple_right_join(tables.first, tables.first->next_local);
    }

    std::string sql;
    bool first= true;
    for (TABLE_LIST *table= tables.first; table; table= table->next_local)
    {
      if (table->outer_join & JOIN_TYPE_RIGHT)
        return unsupported("RIGHT JOIN emission is only implemented for simple two-table joins");
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
        const bool real_outer_join=
            (table->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT)) != 0;
        if (table->on_expr || real_outer_join)
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

  SqlGenerationResult emit_simple_right_join(TABLE_LIST *left_table, TABLE_LIST *right_table)
  {
    if (left_table->nested_join || right_table->nested_join)
      return unsupported("nested RIGHT JOIN emission is not implemented yet");
    if (left_table->table_function || right_table->table_function)
      return unsupported("table function emission is not supported");
    if (left_table->natural_join || left_table->join_using_fields ||
        right_table->natural_join || right_table->join_using_fields)
      return unsupported("NATURAL/USING RIGHT JOIN emission is not implemented yet");
    if (!left_table->on_expr)
      return unsupported("RIGHT JOIN without ON expression is not supported");

    auto right_ref= emit_table_ref(right_table);
    if (!right_ref.supported())
      return right_ref;
    auto left_ref= emit_table_ref(left_table);
    if (!left_ref.supported())
      return left_ref;
    auto condition= emit_expression(left_table->on_expr);
    if (!condition.supported())
      return condition;

    std::string sql= right_ref.sql;
    sql+= " LEFT JOIN ";
    sql+= left_ref.sql;
    sql+= " ON ";
    sql+= condition.sql;
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_table_ref(TABLE_LIST *table)
  {
    std::string sql;

    if (table->derived)
    {
      auto derived= generate_unit(table->derived, false);
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
    const uint outer_join_type= table->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT);
    if (outer_join_type)
      sql+= "LEFT JOIN";
    else
      sql+= "JOIN";

    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_global_order_limit(st_select_lex_unit *lex_unit,
                                              bool suppress_positive_limit)
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

    const bool suppress_limit= should_suppress_positive_limit(parameters, suppress_positive_limit);
    if (!suppress_limit && parameters->limit_params.explicit_limit &&
        parameters->limit_params.select_limit)
    {
      auto limit= emit_integer_constant(parameters->limit_params.select_limit);
      if (!limit.supported())
        return limit;
      sql+= " LIMIT ";
      sql+= limit.sql;
    }

    if (!suppress_limit && parameters->limit_params.explicit_limit &&
        parameters->limit_params.offset_limit)
    {
      auto offset= emit_integer_constant(parameters->limit_params.offset_limit);
      if (!offset.supported())
        return offset;
      sql+= " OFFSET ";
      sql+= offset.sql;
    }

    return SqlGenerationResult::generated(std::move(sql));
  }

  bool should_suppress_positive_limit(st_select_lex *sel_lex, bool suppress_positive_limit)
  {
    return suppress_positive_limit && sel_lex && sel_lex->limit_params.explicit_limit &&
           sel_lex->limit_params.select_limit && !sel_lex->limit_params.offset_limit &&
           is_positive_integer_constant(sel_lex->limit_params.select_limit);
  }

  bool is_positive_integer_constant(Item *item)
  {
    if (!item)
      return false;
    const Item_const *constant= item->get_item_const();
    if (constant)
    {
      if (const longlong *value= constant->const_ptr_longlong())
        return *value > 0;
    }
    return item->const_item() && item->result_type() == INT_RESULT && item->val_int() > 0;
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

  SqlGenerationResult emit_olap_group_prefix(st_select_lex *sel_lex)
  {
    switch (sel_lex->olap)
    {
      case UNSPECIFIED_OLAP_TYPE:
        return SqlGenerationResult::generated(std::string());
      case ROLLUP_TYPE:
        return SqlGenerationResult::generated("ROLLUP(");
      case CUBE_TYPE:
        return unsupported("GROUP BY CUBE emission is not implemented yet");
      default:
        return unsupported("unknown OLAP grouping type");
    }
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
      case Item::COND_ITEM:
        return emit_condition(static_cast<Item_cond *>(item));
      case Item::SUM_FUNC_ITEM:
        return emit_aggregate(static_cast<Item_sum *>(item));
      case Item::WINDOW_FUNC_ITEM:
        return emit_window_function(static_cast<Item_window_func *>(item));
      case Item::SUBSELECT_ITEM:
        return emit_subselect(static_cast<Item_subselect *>(item));
      default:
        return unsupported("unsupported MariaDB Item type");
    }
  }

  SqlGenerationResult emit_ref(Item_ref *item)
  {
    if (!is_empty(item->table_name))
      return emit_identifier(item);
    if (item->ref && *item->ref)
      return emit_expression(*item->ref);
    return emit_identifier(item);
  }

  SqlGenerationResult emit_identifier(Item_ident *item)
  {
    std::string sql;
    const bool alias_reference=
        item->alias_name_used || (item->cached_table && item->cached_table->is_alias);
    if (!alias_reference && !is_empty(item->db_name))
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
    if (const MYSQL_TIME *value= constant->const_ptr_mysql_time())
      return temporal_to_sql(*value, item->decimals);
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

  SqlGenerationResult dispatch_function(Item_func *function)
  {
    // SQLGlot-style dispatch: look up in TRANSFORMS table first
    auto it = function_transforms.find(function->functype());
    if (it != function_transforms.end())
    {
      return it->second(function);
    }
    
    // Fallback: emit function using switch (for gradual migration)
    // TODO: Remove this fallback once all functions migrated to TRANSFORMS
    return emit_function(function);
  }

  // Legacy emit_function - keep for gradual migration
  // TODO: Migrate each case to TRANSFORMS and remove this
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
        return emit_like_function(static_cast<Item_func_like *>(function));
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
      case Item_func::EXTRACT_FUNC:
        return emit_extract_function(static_cast<Item_extract *>(function));
      case Item_func::DATE_FUNC:
        return emit_unary_cast_function(function, "DATE");
      case Item_func::CHAR_TYPECAST_FUNC:
        return emit_char_typecast_function(static_cast<Item_char_typecast *>(function));
      case Item_func::YEAR_FUNC:
        return emit_extract_function(function, "YEAR");
      case Item_func::CASE_SEARCHED_FUNC:
        return emit_searched_case_function(function);
      case Item_func::CASE_SIMPLE_FUNC:
        return emit_simple_case_function(function);
      case Item_func::IN_OPTIMIZER_FUNC:
        return emit_in_optimizer_function(function);
      case Item_func::EQUAL_FUNC:
        return unsupported("NULL-safe equality emission is not implemented yet");
      default:
        return emit_named_or_operator_function(function);
    }
  }

  SqlGenerationResult emit_condition(Item_cond *condition)
  {
    const char *operator_text= nullptr;
    switch (condition->functype())
    {
      case Item_func::COND_AND_FUNC:
        operator_text= "AND";
        break;
      case Item_func::COND_OR_FUNC:
        operator_text= "OR";
        break;
      default:
        return unsupported("unsupported condition function");
    }

    List<Item> *arguments= condition->argument_list();
    if (!arguments || arguments->is_empty())
      return unsupported("condition function has no arguments");

    std::string sql= "(";
    bool first= true;
    List_iterator_fast<Item> iterator(*arguments);
    Item *argument;
    while ((argument= iterator++))
    {
      auto expression= emit_expression(argument);
      if (!expression.supported())
        return expression;
      if (!first)
      {
        sql+= " ";
        sql+= operator_text;
        sql+= " ";
      }
      sql+= expression.sql;
      first= false;
    }
    sql+= ")";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_named_or_operator_function(Item_func *function)
  {
    const LEX_CSTRING name= function->func_name_cstring();
    if (equals_ignore_case(name, "date_add_interval"))
      return emit_date_add_interval_function(static_cast<Item_date_add_interval *>(function));
    if (equals_ignore_case(name, "cast_as_signed"))
      return emit_unary_cast_function(function, "DECIMAL(18,0)");
    if (equals_ignore_case(name, "cast_as_unsigned"))
      return emit_unary_cast_function(function, "DECIMAL(20,0)");
    if (equals_ignore_case(name, "decimal_typecast"))
      return emit_decimal_typecast_function(static_cast<Item_decimal_typecast *>(function));
    if (equals_ignore_case(name, "float_typecast") || equals_ignore_case(name, "double_typecast"))
      return emit_unary_cast_function(function, "DOUBLE");
    if (equals_ignore_case(name, "cast_as_time"))
      return emit_unary_cast_function(function, "TIME");
    if (equals_ignore_case(name, "cast_as_datetime"))
      return emit_unary_cast_function(function, "TIMESTAMP");

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
    if (equals_ignore_case(name, "round"))
      return emit_named_function(function, "ROUND");

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
    std::string reason= "unsupported scalar function";
    if (!is_empty(name))
    {
      reason+= ": ";
      reason+= string_from(name);
    }
    return SqlGenerationResult::unsupported(std::move(reason));
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

  SqlGenerationResult emit_searched_case_function(Item_func *function)
  {
    if (function->argument_count() < 2)
      return unsupported("searched CASE has too few arguments");

    const uint when_count= function->argument_count() / 2;
    const bool with_else= function->argument_count() % 2 == 1;

    std::string sql= "CASE";
    for (uint i= 0; i < when_count; ++i)
    {
      auto condition= emit_expression(function->arguments()[i]);
      if (!condition.supported())
        return condition;
      auto result= emit_expression(function->arguments()[i + when_count]);
      if (!result.supported())
        return result;

      sql+= " WHEN ";
      sql+= condition.sql;
      sql+= " THEN ";
      sql+= result.sql;
    }

    if (with_else)
    {
      auto else_result= emit_expression(function->arguments()[function->argument_count() - 1]);
      if (!else_result.supported())
        return else_result;
      sql+= " ELSE ";
      sql+= else_result.sql;
    }

    sql+= " END";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_simple_case_function(Item_func *function)
  {
    if (function->argument_count() < 3)
      return unsupported("simple CASE has too few arguments");

    const uint when_count= (function->argument_count() - 1) / 2;
    const bool with_else= function->argument_count() % 2 == 0;

    auto value= emit_expression(function->arguments()[0]);
    if (!value.supported())
      return value;

    std::string sql= "CASE ";
    sql+= value.sql;
    for (uint i= 0; i < when_count; ++i)
    {
      auto when_value= emit_expression(function->arguments()[1 + i]);
      if (!when_value.supported())
        return when_value;
      auto result= emit_expression(function->arguments()[1 + when_count + i]);
      if (!result.supported())
        return result;

      sql+= " WHEN ";
      sql+= when_value.sql;
      sql+= " THEN ";
      sql+= result.sql;
    }

    if (with_else)
    {
      auto else_result= emit_expression(function->arguments()[function->argument_count() - 1]);
      if (!else_result.supported())
        return else_result;
      sql+= " ELSE ";
      sql+= else_result.sql;
    }

    sql+= " END";
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

  SqlGenerationResult emit_extract_function(Item_extract *function)
  {
    if (function->argument_count() != 1)
      return unsupported("EXTRACT function has unexpected argument count");

    const char *field_name= interval_unit_name(function->int_type);
    if (!field_name)
      return unsupported("unsupported EXTRACT interval unit");

    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;
    return SqlGenerationResult::generated("EXTRACT(" + std::string(field_name) + " FROM " +
                                          expression.sql + ")");
  }

  SqlGenerationResult emit_unary_cast_function(Item_func *function, const char *type_name)
  {
    if (function->argument_count() != 1)
      return unsupported("CAST has unexpected argument count");
    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;
    return SqlGenerationResult::generated("CAST(" + expression.sql + " AS " +
                                          std::string(type_name) + ")");
  }

  SqlGenerationResult emit_decimal_typecast_function(Item_decimal_typecast *function)
  {
    if (function->argument_count() != 1)
      return unsupported("DECIMAL cast has unexpected argument count");
    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;

    const uint precision=
        my_decimal_length_to_precision(function->max_length, function->decimals,
                                       function->unsigned_flag);
    return SqlGenerationResult::generated("CAST(" + expression.sql + " AS DECIMAL(" +
                                          integer_to_string(precision) + "," +
                                          integer_to_string(function->decimals) + "))");
  }

  SqlGenerationResult emit_char_typecast_function(Item_char_typecast *function)
  {
    if (function->argument_count() != 1)
      return unsupported("CHAR cast has unexpected argument count");
    if (!function->has_explicit_length())
      return unsupported("CHAR cast without explicit length is not supported");

    auto expression= emit_expression(function->arguments()[0]);
    if (!expression.supported())
      return expression;
    return SqlGenerationResult::generated("CAST(" + expression.sql + " AS VARCHAR(" +
                                          integer_to_string(function->get_cast_length()) + "))");
  }

  SqlGenerationResult emit_date_add_interval_function(Item_date_add_interval *function)
  {
    if (function->argument_count() != 2)
      return unsupported("date interval arithmetic has unexpected argument count");

    const char *field_name= interval_unit_name(function->int_type);
    if (!field_name)
      return unsupported("unsupported date interval unit");

    auto date_expression= emit_expression(function->arguments()[0]);
    if (!date_expression.supported())
      return date_expression;
    auto interval_value= emit_interval_literal_value(function->arguments()[1]);
    if (!interval_value.supported())
      return interval_value;

    std::string sql= "(";
    sql+= date_expression.sql;
    sql+= function->date_sub_interval ? " - INTERVAL " : " + INTERVAL ";
    sql+= interval_value.sql;
    sql+= " ";
    sql+= field_name;
    sql+= ")";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_interval_literal_value(Item *item)
  {
    if (!item)
      return unsupported("missing interval literal");

    const Item_const *constant= item->get_item_const();
    if (!constant || constant->const_is_null())
      return unsupported("non-constant interval expression is not supported");

    if (const longlong *value= constant->const_ptr_longlong())
      return SqlGenerationResult::generated("'" + integer_to_string(*value) + "'");
    if (const my_decimal *value= constant->const_ptr_my_decimal())
    {
      auto decimal= decimal_to_sql(*value);
      if (!decimal.supported())
        return decimal;
      return SqlGenerationResult::generated("'" + decimal.sql + "'");
    }
    if (const String *value= constant->const_ptr_string())
      return SqlGenerationResult::generated(quote_string(*value));

    return unsupported("unsupported interval literal");
  }

  const char *interval_unit_name(interval_type type)
  {
    switch (type)
    {
      case INTERVAL_YEAR:
        return "YEAR";
      case INTERVAL_MONTH:
        return "MONTH";
      case INTERVAL_DAY:
        return "DAY";
      case INTERVAL_HOUR:
        return "HOUR";
      case INTERVAL_MINUTE:
        return "MINUTE";
      case INTERVAL_SECOND:
        return "SECOND";
      default:
        return nullptr;
    }
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

  SqlGenerationResult emit_like_function(Item_func_like *function)
  {
    if (function->argument_count() != 2)
      return unsupported("LIKE function has unexpected argument count");
    auto left= emit_expression(function->arguments()[0]);
    if (!left.supported())
      return left;
    auto right= emit_expression(function->arguments()[1]);
    if (!right.supported())
      return right;

    return SqlGenerationResult::generated("(" + left.sql +
                                          (function->get_negated() ? " NOT LIKE " : " LIKE ") +
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

  SqlGenerationResult emit_in_optimizer_function(Item_func *function)
  {
    if (function->argument_count() != 2)
      return unsupported("IN optimizer has unexpected argument count");
    return emit_expression(function->arguments()[1]);
  }

  SqlGenerationResult emit_subselect(Item_subselect *subselect)
  {
    if (!subselect || !subselect->unit)
      return unsupported("subquery has no SELECT unit");

    const bool suppress_exists_limit= subselect->substype() == Item_subselect::EXISTS_SUBS;
    auto unit= generate_unit(subselect->unit, suppress_exists_limit);
    if (!unit.supported())
      return unit;

    switch (subselect->substype())
    {
      case Item_subselect::SINGLEROW_SUBS:
        return SqlGenerationResult::generated("(" + unit.sql + ")");
      case Item_subselect::EXISTS_SUBS:
        return SqlGenerationResult::generated("EXISTS (" + unit.sql + ")");
      case Item_subselect::IN_SUBS:
        return emit_in_subselect(subselect, unit.sql);
      default:
        return unsupported("unsupported subquery type");
    }
  }

  SqlGenerationResult emit_in_subselect(Item_subselect *subselect, const std::string &unit_sql)
  {
    Item_in_subselect *in_subselect= subselect->get_IN_subquery();
    if (!in_subselect)
      return unsupported("IN subquery has no left expression");

    auto left= emit_expression(in_subselect->left_exp());
    if (!left.supported())
      return left;

    return SqlGenerationResult::generated("(" + left.sql + " IN (" + unit_sql + "))");
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
      case Item_sum::STD_FUNC:
        function_name= static_cast<Item_sum_std *>(aggregate)->sample ? "STDDEV_SAMP" : "STDDEV";
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

  SqlGenerationResult emit_window_function(Item_window_func *window)
  {
    if (!window || !window->window_func())
      return unsupported("window function has no aggregate function");
    if (!window->window_spec)
      return unsupported("named window references are not implemented yet");

    auto function= emit_window_function_call(window->window_func());
    if (!function.supported())
      return function;
    auto specification= emit_window_spec(window->window_spec);
    if (!specification.supported())
      return specification;

    return SqlGenerationResult::generated(function.sql + " OVER " + specification.sql);
  }

  SqlGenerationResult emit_window_function_call(Item_sum *function)
  {
    const char *function_name= nullptr;
    bool distinct= false;
    bool empty_argument_list= false;

    switch (function->sum_func())
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
      case Item_sum::STD_FUNC:
        function_name= static_cast<Item_sum_std *>(function)->sample ? "STDDEV_SAMP" : "STDDEV";
        break;
      case Item_sum::ROW_NUMBER_FUNC:
        function_name= "ROW_NUMBER";
        empty_argument_list= true;
        break;
      case Item_sum::RANK_FUNC:
        function_name= "RANK";
        empty_argument_list= true;
        break;
      case Item_sum::DENSE_RANK_FUNC:
        function_name= "DENSE_RANK";
        empty_argument_list= true;
        break;
      default:
        return unsupported("unsupported window function");
    }

    std::string sql= std::string(function_name) + "(";
    if (distinct)
      sql+= "DISTINCT ";

    if (empty_argument_list)
    {
      if (function->argument_count() != 0)
        return unsupported("ranking window function unexpectedly has arguments");
    }
    else if (function->argument_count() == 0)
      sql+= "*";
    else
    {
      for (uint i= 0; i < function->argument_count(); ++i)
      {
        auto expression= emit_expression(function->arguments()[i]);
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

  SqlGenerationResult emit_window_spec(Window_spec *specification)
  {
    std::string sql= "(";
    bool need_space= false;

    if (specification->partition_list && specification->partition_list->elements)
    {
      auto partition_by= emit_order_list(specification->partition_list->first, false);
      if (!partition_by.supported())
        return partition_by;
      sql+= "PARTITION BY ";
      sql+= partition_by.sql;
      need_space= true;
    }

    if (specification->order_list && specification->order_list->elements)
    {
      auto order_by= emit_order_list(specification->order_list->first, true);
      if (!order_by.supported())
        return order_by;
      if (need_space)
        sql+= " ";
      sql+= "ORDER BY ";
      sql+= order_by.sql;
      need_space= true;
    }

    if (specification->window_frame)
    {
      auto frame= emit_window_frame(specification->window_frame);
      if (!frame.supported())
        return frame;
      if (need_space)
        sql+= " ";
      sql+= frame.sql;
    }

    sql+= ")";
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_window_frame(Window_frame *frame)
  {
    if (frame->exclusion != Window_frame::EXCL_NONE)
      return unsupported("window frame exclusion is not implemented yet");

    auto top_bound= emit_window_frame_bound(frame->top_bound);
    if (!top_bound.supported())
      return top_bound;
    auto bottom_bound= emit_window_frame_bound(frame->bottom_bound);
    if (!bottom_bound.supported())
      return bottom_bound;

    std::string sql= frame->units == Window_frame::UNITS_ROWS ? "ROWS" : "RANGE";
    sql+= " BETWEEN ";
    sql+= top_bound.sql;
    sql+= " AND ";
    sql+= bottom_bound.sql;
    return SqlGenerationResult::generated(std::move(sql));
  }

  SqlGenerationResult emit_window_frame_bound(Window_frame_bound *bound)
  {
    if (!bound)
      return unsupported("missing window frame bound");

    if (bound->precedence_type == Window_frame_bound::CURRENT)
      return SqlGenerationResult::generated("CURRENT ROW");

    if (bound->is_unbounded())
    {
      if (bound->precedence_type == Window_frame_bound::PRECEDING)
        return SqlGenerationResult::generated("UNBOUNDED PRECEDING");
      if (bound->precedence_type == Window_frame_bound::FOLLOWING)
        return SqlGenerationResult::generated("UNBOUNDED FOLLOWING");
      return unsupported("unbounded CURRENT ROW window frame bound is invalid");
    }

    auto offset= emit_expression(bound->offset);
    if (!offset.supported())
      return offset;
    std::string sql= offset.sql;
    sql+= bound->precedence_type == Window_frame_bound::PRECEDING ? " PRECEDING" : " FOLLOWING";
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

  // Initialize TRANSFORMS tables (called once at startup)
  static void init_transforms()
  {
    // Function transforms - mirrors SQLGlot EXASOLGenerator.TRANSFORMS
    function_transforms = {
      // Comparison operators
      {Item_func::EQ_FUNC, [](Item_func *f) { return emit_binary_function(f, "="); }},
      {Item_func::NE_FUNC, [](Item_func *f) { return emit_binary_function(f, "<>"); }},
      {Item_func::LT_FUNC, [](Item_func *f) { return emit_binary_function(f, "<"); }},
      {Item_func::LE_FUNC, [](Item_func *f) { return emit_binary_function(f, "<="); }},
      {Item_func::GE_FUNC, [](Item_func *f) { return emit_binary_function(f, ">="); }},
      {Item_func::GT_FUNC, [](Item_func *f) { return emit_binary_function(f, ">"); }},
      
      // Add more here: like SQLGlot pattern
      // {Item_func::BETWEEN, [](Item_func *f) { return emit_between_function(f); }},
      // {Item_func::LIKE_FUNC, [](Item_func *f) { return emit_like_function(f); }},
    };
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
