#ifndef EXASOL_GW_SQL_GENERATOR_INCLUDED
#define EXASOL_GW_SQL_GENERATOR_INCLUDED

#include <string>
#include <utility>
#include <functional>

class THD;
struct st_order;
class st_select_lex;
class st_select_lex_unit;
class Item;
class Item_func;
class Item_sum;
class Item_window_func;
class Item_subselect;

namespace exasol_gw
{

struct SqlGenerationResult
{
  std::string sql;
  std::string unsupported_reason;

  bool supported() const { return unsupported_reason.empty(); }

  static SqlGenerationResult generated(std::string sql_text)
  {
    SqlGenerationResult result;
    result.sql= std::move(sql_text);
    return result;
  }

  static SqlGenerationResult unsupported(std::string reason)
  {
    SqlGenerationResult result;
    result.unsupported_reason= std::move(reason);
    return result;
  }
};

typedef std::function<SqlGenerationResult(Item_func *)> FuncTransform;
typedef std::function<SqlGenerationResult(Item_sum *)> AggregateTransform;
typedef std::function<SqlGenerationResult(Item_window_func *)> WindowTransform;
typedef std::function<SqlGenerationResult(Item_subselect *)> SubqueryTransform;

SqlGenerationResult generate_exasol_sql(THD *thd, st_select_lex_unit *lex_unit);
SqlGenerationResult generate_exasol_sql(THD *thd, st_select_lex *sel_lex);
SqlGenerationResult generate_exasol_order_sql(THD *thd, st_order *order);

} // namespace exasol_gw

#endif
