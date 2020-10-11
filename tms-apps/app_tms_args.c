#include "asterisk.h"

#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"

static const char app[] = "TMSArgs";

enum args_app_options
{
  OPT_ONE = (1 << 0),
  OPT_TWO = (1 << 1),
  OPT_THERE = (1 << 2),
};

AST_APP_OPTIONS(
    args_app_options,
    {
        AST_APP_OPTION('a', OPT_ONE),
        AST_APP_OPTION('b', OPT_TWO),
        AST_APP_OPTION('c', OPT_THERE),
    });

static int args_exec(struct ast_channel *chan, const char *data)
{
  int res = 0;
  char *argcopy = NULL;
  struct ast_flags app_options = {0};

  /* 声明应用支持的参数 */
  AST_DECLARE_APP_ARGS(
      arglist,
      AST_APP_ARG(str);
      AST_APP_ARG(options);
      AST_APP_ARG(arr););

  ast_debug(1, "进入TMSArgs data = %s\n", data);

  /* 解析参数 */
  argcopy = ast_strdupa(data);
  AST_STANDARD_APP_ARGS(arglist, argcopy);

  /* 字符串参数 */
  if (!ast_strlen_zero(arglist.str))
  {
    ast_debug(1, "参数1：str = %s\n", arglist.str);
  }

  /* 标志位参数 */
  if (!ast_strlen_zero(arglist.options))
  {
    ast_app_parse_options(args_app_options, &app_options, NULL, arglist.options);
    ast_debug(1, "参数2：options = %02x\n", app_options.flags);
  }

  /* &分割的数组 */
  if (!ast_strlen_zero(arglist.arr))
  {
    char *str_in_arr;
    int i = 0;
    while (NULL != (str_in_arr = strsep(&arglist.arr, "&")))
    {
      ast_debug(1, "参数3：arr[%d] = %s\n", i, str_in_arr);
      i++;
    }
  }

  ast_debug(1, "离开TMSArgs\n");

  return res;
}

static int unload_module(void)
{
  return ast_unregister_application(app);
}

static int load_module(void)
{
  return ast_register_application_xml(app, args_exec);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "TMS Args Application");
