/*
  Copyright (c) 2020, MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h" /* TMP_TABLE_PARAM */
#include "table.h"
#include "item_jsonfunc.h"
#include "table_function.h"


class table_function_handlerton
{
public:
  handlerton m_hton;
  table_function_handlerton()
  {
    bzero(&m_hton, sizeof(m_hton));
    m_hton.tablefile_extensions= hton_no_exts;
    m_hton.slot= HA_SLOT_UNDEF;
  }
};


static table_function_handlerton table_function_hton;


class ha_json_table: public handler
{
protected:
  Table_function_json_table *m_jt;
  String m_tmps;
  String *m_js;
  json_engine_t m_je;
  longlong m_order;
  json_path_t m_cur_path;

public:
  ha_json_table(TABLE_SHARE *share_arg, Table_function_json_table *jt):
    handler(&table_function_hton.m_hton, share_arg), m_jt(jt)
  {
    mark_trx_read_write_done= 1;
  }
  ~ha_json_table() {}
  handler *clone(const char *name, MEM_ROOT *mem_root) { return NULL; }
  const char *index_type(uint inx) { return "HASH"; }
  /* Rows also use a fixed-size format */
  enum row_type get_row_type() const { return ROW_TYPE_FIXED; }
  ulonglong table_flags() const
  {
    return (HA_FAST_KEY_READ | HA_NO_BLOBS | HA_NULL_IN_KEY |
            HA_CAN_SQL_HANDLER |
            HA_REC_NOT_IN_SEQ | HA_NO_TRANSACTIONS |
            HA_HAS_RECORDS | HA_CAN_HASH_KEYS);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    return HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR;
  }
  uint max_supported_keys() const { return 1; }
  uint max_supported_key_part_length() const { return MAX_KEY_LENGTH; }
  double scan_time() { return 1000000.0; }
  double read_time(uint index, uint ranges, ha_rows rows) { return 0.0; }

  int open(const char *name, int mode, uint test_if_locked);
  int close(void) { return 0; }
  int index_read_map(uchar * buf, const uchar * key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag);
  int index_next(uchar * buf);
  int rnd_init(bool scan);
  int rnd_next(uchar *buf);
  int rnd_pos(uchar * buf, uchar *pos) { return 1; }
  void position(const uchar *record) {}
  int can_continue_handler_scan() { return 1; }
  int info(uint);
  int extra(enum ha_extra_function operation);
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
			     enum thr_lock_type lock_type)
    { return NULL; }
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info)
    { return 1; }
private:
  void update_key_stats();
protected:
  int index_next_same(uchar *buf, const uchar *key, uint keylen)
    { return index_next(buf); }
};


class Create_json_table: public Data_type_statistics
{
  // The following members are initialized only in start()
  Field **m_default_field;
  KEY_PART_INFO *m_key_part_info;
  uchar	*m_bitmaps;
  // The following members are initialized in ctor
  uint m_temp_pool_slot;
  uint m_null_count;
  Field *m_index_field;
public:
  Create_json_table(const TMP_TABLE_PARAM *param,
                    bool save_sum_fields)
   :m_temp_pool_slot(MY_BIT_NONE),
    m_null_count(0)
  { }

  void add_field(TABLE *table, Field *field, uint fieldnr, bool force_not_null_cols);

  TABLE *start(THD *thd,
               TMP_TABLE_PARAM *param,
               Table_function_json_table *jt,
               const LEX_CSTRING *table_alias);

  bool add_json_table_fields(THD *thd, TABLE *table,
                             Table_function_json_table *jt);
  bool finalize(THD *thd, TABLE *table, TMP_TABLE_PARAM *param,
                Table_function_json_table *jt);
};


static const LEX_CSTRING jfield_name= { "JSON_SOURCE", 11 };
static const LEX_CSTRING jindex_name= { "JSON_SOURCE_index", 17 };

int ha_json_table::open(const char *name, int mode, uint test_if_locked)
{
  return 0;
}


int ha_json_table::extra(enum ha_extra_function operation)
{
  return 0;
}


int ha_json_table::index_read_map(uchar * buf,
                         const uchar * key, key_part_map keypart_map,
                         enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_json_table::index_read_map");
  rnd_init(true);
  DBUG_RETURN(rnd_next(buf));
}


int ha_json_table::index_next(uchar * buf)
{
  return rnd_next(buf);
}


int ha_json_table::rnd_init(bool scan)
{
  DBUG_ENTER("ha_json_table::index_read_map");

  if ((m_js= m_jt->m_json->val_str(&m_tmps)))
  {
    json_get_path_start(&m_je, m_js->charset(),
      (const uchar *) m_js->ptr(), (const uchar *) m_js->end(), &m_cur_path);
    m_order= 0;
  }

  DBUG_RETURN(0);
}

int ha_json_table::rnd_next(uchar *buf)
{
  Field **f= table->field;
  Json_table_column *jc;

  if (!m_js)
    return HA_ERR_END_OF_FILE;

  while (!json_get_path_next(&m_je, &m_cur_path))
  {
    if (json_path_compare(&m_jt->m_path, &m_cur_path, m_je.value_type))
      continue;
    goto path_found;
  }

  return HA_ERR_END_OF_FILE;
  
path_found:
  (*f)->set_null();

  List_iterator_fast<Json_table_column> jc_i(m_jt->m_columns);
  while ((jc= jc_i++))
  {
    f++;
    switch (jc->m_column_type)
    {
    case Json_table_column::FOR_ORDINALITY:
      (*f)->set_notnull();
      (*f)->store(m_order++, TRUE);
      break;
    case Json_table_column::PATH:
      json_engine_t je;
      json_path_step_t *cur_step;
      uint array_counters[JSON_DEPTH_LIMIT];
      json_scan_start(&je, m_js->charset(),
          m_je.value_begin, m_je.s.str_end);

      cur_step= jc->m_path.steps;
      if (json_find_path(&je, &jc->m_path, &cur_step, array_counters) ||
          json_read_value(&je))
        (*f)->set_null();
      else
      {
        (*f)->set_notnull();
        (*f)->store((const char *) je.value, (uint32) je.value_len,je.s.cs);
      }
      break;
    default:
      return HA_ERR_END_OF_FILE;
    };
  }
  return 0;
}


int ha_json_table::info(uint)
{
  errkey= 1;
  stats.records= 40;
  stats.deleted= 0;
  stats.mean_rec_length= 140;
  stats.data_file_length= 127008;
  stats.index_file_length= 126984;
  stats.max_data_file_length= 14679980;
  stats.delete_length= 0;
  stats.create_time= 1584951202;
  stats.auto_increment_value= 0;
  table->s->key_info[0].rec_per_key[0]= 3;
  return 0;
}


static void
setup_tmp_table_column_bitmaps(TABLE *table, uchar *bitmaps, uint field_count)
{
  uint bitmap_size= bitmap_buffer_size(field_count);

  DBUG_ASSERT(table->s->virtual_fields == 0);

  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count,
              FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->tmp_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->eq_join_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->cond_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->has_value_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  /* write_set and all_set are copies of read_set */
  table->def_write_set= table->def_read_set;
  table->s->all_set= table->def_read_set;
  bitmap_set_all(&table->s->all_set);
  table->default_column_bitmaps();
}


void Create_json_table::add_field(TABLE *table, Field *field,
                                  uint fieldnr, bool force_not_null_cols)
{
  DBUG_ASSERT(!field->field_name.str ||
              strlen(field->field_name.str) == field->field_name.length);

  if (force_not_null_cols)
  {
    field->flags|= NOT_NULL_FLAG;
    field->null_ptr= NULL;
  }

  if (!(field->flags & NOT_NULL_FLAG))
    m_null_count++;

  table->s->reclength+= field->pack_length();

  // Assign it here, before update_data_type_statistics() changes m_blob_count
  if (field->flags & BLOB_FLAG)
    table->s->blob_field[m_blob_count]= fieldnr;

  table->field[fieldnr]= field;
  field->field_index= fieldnr;

  field->update_data_type_statistics(this);
}


/**
  Create a json table according to a field list.

  @param thd                  thread handle
  @param param                a description used as input to create the table
  @param jt                   json_table specificaion
  @param table_alias          alias
*/

TABLE *Create_json_table::start(THD *thd,
                               TMP_TABLE_PARAM *param,
                               Table_function_json_table *jt,
                               const LEX_CSTRING *table_alias)
{
  MEM_ROOT *mem_root_save, own_root;
  TABLE *table;
  TABLE_SHARE *share;
  uint  copy_func_count= param->func_count;
  char  *tmpname,path[FN_REFLEN];
  Field **reg_field;
  uint *blob_field;
  DBUG_ENTER("Create_json_table::start");
  DBUG_PRINT("enter",
             ("table_alias: '%s'  ", table_alias->str));

  if (use_temp_pool && !(test_flags & TEST_KEEP_TMP_TABLES))
    m_temp_pool_slot = bitmap_lock_set_next(&temp_pool);

  if (m_temp_pool_slot != MY_BIT_NONE) // we got a slot
    sprintf(path, "%s-%lx-%i", tmp_file_prefix,
            current_pid, m_temp_pool_slot);
  else
  {
    /* if we run out of slots or we are not using tempool */
    sprintf(path, "%s-%lx-%lx-%x", tmp_file_prefix,current_pid,
            (ulong) thd->thread_id, thd->tmp_table++);
  }

  /*
    No need to change table name to lower case as we are only creating
    MyISAM, Aria or HEAP tables here
  */
  fn_format(path, path, mysql_tmpdir, "",
            MY_REPLACE_EXT|MY_UNPACK_FILENAME);

  const uint field_count= param->field_count;
  DBUG_ASSERT(field_count);

  init_sql_alloc(&own_root, "tmp_table", TABLE_ALLOC_BLOCK_SIZE, 0,
                 MYF(MY_THREAD_SPECIFIC));

  if (!multi_alloc_root(&own_root,
                        &table, sizeof(*table),
                        &share, sizeof(*share),
                        &reg_field, sizeof(Field*) * (field_count+1),
                        &m_default_field, sizeof(Field*) * (field_count),
                        &blob_field, sizeof(uint)*(field_count+1),
                        &param->items_to_copy,
                          sizeof(param->items_to_copy[0])*(copy_func_count+1),
                        &param->keyinfo, sizeof(*param->keyinfo),
                        &m_key_part_info, sizeof(*m_key_part_info)*(1+1),
                        &param->start_recinfo,
                        sizeof(*param->recinfo)*(field_count*2+4),
                        &tmpname, (uint) strlen(path)+1,
                        &m_bitmaps, bitmap_buffer_size(field_count)*6,
                        NullS))
  {
    DBUG_RETURN(NULL);				/* purecov: inspected */
  }
  strmov(tmpname, path);
  /* make table according to fields */

  bzero((char*) table,sizeof(*table));
  bzero((char*) reg_field, sizeof(Field*) * (field_count+1));
  bzero((char*) m_default_field, sizeof(Field*) * (field_count));

  table->mem_root= own_root;
  mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  table->field=reg_field;
  table->alias.set(table_alias->str, table_alias->length, table_alias_charset);

  table->reginfo.lock_type=TL_WRITE;	/* Will be updated */
  table->map=1;
  table->temp_pool_slot= m_temp_pool_slot;
  table->copy_blobs= 1;
  table->in_use= thd;
  table->no_rows_with_nulls= param->force_not_null_cols;
  table->update_handler= NULL;
  table->check_unique_buf= NULL;

  table->s= share;
  init_tmp_table_share(thd, share, "", 0, "(temporary)", tmpname);
  share->blob_field= blob_field;
  share->table_charset= param->table_charset;
  share->primary_key= MAX_KEY;               // Indicate no primary key
  if (param->schema_table)
    share->db= INFORMATION_SCHEMA_NAME;

  param->using_outer_summary_function= 0;
  thd->mem_root= mem_root_save;
  DBUG_RETURN(table);
}


bool Create_json_table::finalize(THD *thd, TABLE *table,
                                 TMP_TABLE_PARAM *param,
                                 Table_function_json_table *jt)
{
  DBUG_ENTER("Create_json_table::finalize");
  DBUG_ASSERT(table);

  uint null_pack_length;
  bool  use_packed_rows= false;
  uchar *pos;
  uchar *null_flags;
  TMP_ENGINE_COLUMNDEF *recinfo;
  TABLE_SHARE  *share= table->s;

  MEM_ROOT *mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  DBUG_ASSERT(param->field_count >= share->fields);
  DBUG_ASSERT(param->field_count >= share->blob_fields);

  share->db_plugin= NULL;
  if (!(table->file= new (&table->mem_root) ha_json_table(share, jt)))
    goto err;

  if (table->file->set_ha_share_ref(&share->ha_share))
  {
    delete table->file;
    goto err;
  }

  if (share->blob_fields == 0)
    m_null_count++;

  null_pack_length= (m_null_count + m_uneven_bit_length + 7) / 8;
  share->reclength+= null_pack_length;
  if (!share->reclength)
    share->reclength= 1;                // Dummy select

  {
    uint alloc_length= ALIGN_SIZE(share->reclength + MI_UNIQUE_HASH_LENGTH+1);
    share->rec_buff_length= alloc_length;
    if (!(table->record[0]= (uchar*)
                            alloc_root(&table->mem_root, alloc_length*3)))
      goto err;
    table->record[1]= table->record[0]+alloc_length;
    share->default_values= table->record[1]+alloc_length;
  }

  setup_tmp_table_column_bitmaps(table, m_bitmaps, table->s->fields);

  recinfo=param->start_recinfo;
  null_flags=(uchar*) table->record[0];
  pos=table->record[0]+ null_pack_length;
  if (null_pack_length)
  {
    bzero((uchar*) recinfo,sizeof(*recinfo));
    recinfo->type=FIELD_NORMAL;
    recinfo->length=null_pack_length;
    recinfo++;
    bfill(null_flags,null_pack_length,255);	// Set null fields

    table->null_flags= (uchar*) table->record[0];
    share->null_fields= m_null_count;
    share->null_bytes= share->null_bytes_for_compare= null_pack_length;
  }
  m_null_count= (share->blob_fields == 0) ? 1 : 0;
  for (uint i= 0; i < share->fields; i++, recinfo++)
  {
    Field *field= table->field[i];
    uint length;
    bzero((uchar*) recinfo,sizeof(*recinfo));

    if (!(field->flags & NOT_NULL_FLAG))
    {
      recinfo->null_bit= (uint8)1 << (m_null_count & 7);
      recinfo->null_pos= m_null_count/8;
      field->move_field(pos, null_flags + m_null_count/8,
			(uint8)1 << (m_null_count & 7));
      m_null_count++;
    }
    else
      field->move_field(pos,(uchar*) 0,0);
    if (field->type() == MYSQL_TYPE_BIT)
    {
      /* We have to reserve place for extra bits among null bits */
      ((Field_bit*) field)->set_bit_ptr(null_flags + m_null_count / 8,
                                        m_null_count & 7);
      m_null_count+= (field->field_length & 7);
    }
    field->reset();

    /*
      Test if there is a default field value. The test for ->ptr is to skip
      'offset' fields generated by initialize_tables
    */
    if (m_default_field[i] && m_default_field[i]->ptr)
    {
      /* 
         default_field[i] is set only in the cases  when 'field' can
         inherit the default value that is defined for the field referred
         by the Item_field object from which 'field' has been created.
      */
      const Field *orig_field= m_default_field[i];
      /* Get the value from default_values */
      if (orig_field->is_null_in_record(orig_field->table->s->default_values))
        field->set_null();
      else
      {
        field->set_notnull();
        memcpy(field->ptr,
               orig_field->ptr_in_record(orig_field->table->s->default_values),
               field->pack_length_in_rec());
      }
    } 

    length=field->pack_length();
    pos+= length;

    /* Make entry for create table */
    recinfo->length=length;
    recinfo->type= field->tmp_engine_column_type(use_packed_rows);
    m_null_count= (m_null_count + 7) & ~7;    // move to next byte

    // fix table name in field entry
    field->set_table_name(&table->alias);
  }

  param->recinfo= recinfo;              	// Pointer to after last field
  store_record(table,s->default_values);        // Make empty default record

  share->max_rows= ~(ha_rows) 0;
  param->end_write_records= HA_POS_ERROR;

  {
    /* add the 'virtual' key */
    KEY *keyinfo= param->keyinfo;
    DBUG_PRINT("info",("Creating group key in temporary table"));
    share->keys=1;
    share->key_parts= 1;
    share->uniques= 0;
    table->key_info= table->s->key_info= keyinfo;
    table->keys_in_use_for_query.set_bit(0);
    share->keys_in_use.set_bit(0);
    keyinfo->key_part= m_key_part_info;
    keyinfo->name= jindex_name;
    keyinfo->flags= HA_BINARY_PACK_KEY | HA_PACK_KEY;
    keyinfo->ext_key_flags= keyinfo->flags;
    keyinfo->usable_key_parts=keyinfo->user_defined_key_parts= 1;
    keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
    keyinfo->key_length=0;
    keyinfo->rec_per_key=
      (ulong*) thd->calloc(sizeof(ulong)*share->key_parts);
    keyinfo->read_stats= NULL;
    keyinfo->collected_stats= NULL;
    keyinfo->algorithm= HA_KEY_ALG_UNDEF;
    keyinfo->is_statistics_from_stat_tables= FALSE;
    {
      m_key_part_info->field= m_index_field;
      m_key_part_info->null_bit=m_index_field->null_bit;
      m_key_part_info->null_offset=
        (uint) (m_index_field->null_ptr - (uchar*) table->record[0]);
      m_key_part_info->field= m_index_field;
      m_key_part_info->fieldnr= m_index_field->field_index + 1;
      m_index_field->key_start.set_bit(0);
      m_key_part_info->offset= m_index_field->offset(table->record[0]);
      m_key_part_info->length= (uint16) m_index_field->key_length();
      m_key_part_info->store_length= (uint16) m_index_field->key_length();
      m_key_part_info->type=   (uint8) m_index_field->key_type();
      m_key_part_info->key_type= 0;
      m_key_part_info->key_part_flag= 0;
      keyinfo->key_length+=  m_key_part_info->length;
    }
  }

  share->db_record_offset= 1;

  if (unlikely(table->file->ha_open(table, table->s->path.str, O_RDWR,
                             HA_OPEN_TMP_TABLE | HA_OPEN_INTERNAL_TABLE)))
    goto err;

  table->db_stat= HA_OPEN_KEYFILE;
  table->set_created();

  thd->mem_root= mem_root_save;

  DBUG_RETURN(false);

err:
  thd->mem_root= mem_root_save;
  DBUG_RETURN(true);
}


bool Create_json_table::add_json_table_fields(THD *thd, TABLE *table,
                                              Table_function_json_table *jt)
{
  TABLE_SHARE *share= table->s;
  Json_table_column *jc;
  uint fieldnr= 0;
  MEM_ROOT *mem_root_save= thd->mem_root;

  DBUG_ENTER("add_json_table_fields");

  {
    /* Add the invisible json_string field. */
    Field *f= new (thd->mem_root) Field_string(0,
        my_charset_utf8mb4_bin.mbmaxlen,(uchar *) "", 0,
        Field::NONE, &jfield_name, &my_charset_utf8mb4_bin);
    f->invisible= INVISIBLE_USER;
    f->init(table);
    add_field(table, f, fieldnr++, FALSE);
    m_index_field= f;
  }

  thd->mem_root= &table->mem_root;
  List_iterator_fast<Json_table_column> jc_i(jt->m_columns);
  while ((jc= jc_i++))
  {
    Create_field *sql_f= jc->m_field;
    Record_addr addr(!(sql_f->flags && NOT_NULL_FLAG));
    Bit_addr bit(addr.null());

    if (!sql_f->charset)
      sql_f->charset= &my_charset_utf8mb4_bin;

    Field *f= sql_f->type_handler()->make_table_field_from_def(share,
        thd->mem_root, &sql_f->field_name, addr, bit, sql_f, sql_f->flags);
    if (!f)
      DBUG_RETURN(TRUE);
    f->init(table);
    add_field(table, f, fieldnr++, FALSE);
  }

  share->fields= fieldnr;
  share->blob_fields= m_blob_count;
  table->field[fieldnr]= 0;                     // End marker
  share->blob_field[m_blob_count]= 0;           // End marker
  share->column_bitmap_size= bitmap_buffer_size(share->fields);

  thd->mem_root= mem_root_save;

  DBUG_RETURN(0);
}


TABLE *create_table_for_function(THD *thd, TABLE_LIST *sql_table)
{
  TMP_TABLE_PARAM tp;
  TABLE *table;
  uint field_count= sql_table->table_function->m_columns.elements + 1;
  
  DBUG_ENTER("create_table_for_function");

  tp.init();
  tp.table_charset= system_charset_info;
  tp.field_count= field_count;
  {
    Create_json_table maker(&tp, false);

    if (!(table= maker.start(thd, &tp,
                             sql_table->table_function, &sql_table->alias)) ||
        maker.add_json_table_fields(thd, table, sql_table->table_function) ||
        maker.finalize(thd, table, &tp, sql_table->table_function))
    {
      DBUG_RETURN(NULL);
    }
  }
  sql_table->schema_table_name.length= 0;

  my_bitmap_map* bitmaps=
    (my_bitmap_map*) thd->alloc(bitmap_buffer_size(field_count));
  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count,
                 FALSE);
  table->read_set= &table->def_read_set;
  bitmap_clear_all(table->read_set);
  table->alias_name_used= true;
  table->next= thd->derived_tables;
  thd->derived_tables= table;
  table->s->tmp_table= INTERNAL_TMP_TABLE;
  table->grant.privilege= SELECT_ACL;

  sql_table->table= table;

  DBUG_RETURN(table);
}


int Json_table_column::set(THD *thd, enum_type ctype, const LEX_CSTRING &path)
{
  set(ctype);
  if (json_path_setup(&m_path, thd->variables.collation_connection,
        (const uchar *) path.str, (const uchar *)(path.str + path.length)))
  {
    report_path_error_ex(path.str, &m_path, "JSON_TABLE", 1,
                         Sql_condition::WARN_LEVEL_ERROR);
    return 1;
  }
  return 0;
}


int Table_function_json_table::set_path(THD *thd, const LEX_CSTRING &path)
{
  if (json_path_setup(&m_path, thd->variables.collation_connection,
        (const uchar *) path.str, (const uchar *)(path.str + path.length)))
  {
    report_path_error_ex(path.str, &m_path, "JSON_TABLE", 1,
                         Sql_condition::WARN_LEVEL_ERROR);
    return 1;
  }
  return 0;
}


int Table_function_json_table::setup(THD *thd, TABLE_LIST *sql_table,
                                     COND **cond)
{
  if (m_json->fix_fields(thd, &m_json))
    return TRUE;

  m_dep_tables= m_json->used_tables();

  if (m_dep_tables)
  {
    /*
      Since the json value depends on other tables, we add the
      AND 'JSON_SOURCE=json_expression' condition to the WHERE
      to make the optimizer using the JSON_SOURCE_index key
      and call ::index_read / ::index_next methods.
    */
    Item_func_eq *eq;
    sql_table->dep_tables|= m_dep_tables;
    sql_table->table->no_cache= TRUE;
    if (unlikely(sql_table->dep_tables & sql_table->get_map()))
    {
      /* Table itself is used in the argument. */
      my_error(ER_WRONG_USAGE, MYF(0), "JSON_TABLE", "argument"); 
      return TRUE;
    }
    eq= new (thd->mem_root) Item_func_eq(thd,
         new (thd->mem_root) Item_field(thd,
                                        &thd->lex->current_select->context,
                                        sql_table->db, sql_table->alias,
                                        jfield_name),
         m_json);
    *cond= and_conds(thd, *cond, eq);
    if (!(*cond) ||
        (*cond)->fix_fields_if_needed_for_bool(thd, cond))
      return TRUE;
    eq->set_always_equal();
  }
  else
  {
    /*
      Nothing to do.
      Let the optimizer call ::rnd_init / ::rnd_next
    */
  }

  return FALSE;
}

