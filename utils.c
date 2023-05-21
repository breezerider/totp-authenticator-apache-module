#include "utils.h"

#include "ap_config.h"

/**
  * Based on the given username, get the users TOTP configuration
 **/
totp_user_config *
totp_read_user_config(const char *user, const char *token_dir, apr_pool_t *pool)
{
	const char       *psep = " ";
	char             *config_filename;
	char             *token, *last;
	char              line[MAX_STRING_LEN];
	char              err_char;
	unsigned int      line_len = 0, line_no = 0;
	apr_status_t      status;
	ap_configfile_t  *config_file;
	totp_user_config *user_config = NULL;

	config_filename = apr_psprintf(pool, "%s/%s", token_dir, username);

	status = ap_pcfg_openfile(&config_file, pool, config_filename);

	if (status != APR_SUCCESS) {
		ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
			      "read_user_totp_config: could not open user configuration file: %s",
			      config_filename);
		return NULL;
	}

	user_config = apr_palloc(pool, sizeof(*user_config));
	memset(user_config, 0, sizeof(*user_config));

	while (!(ap_cfg_getline(line, MAX_STRING_LEN, config_file))) {
		/* Bump line number counter */
		line_no++;
		/* Skip blank lines. */
		if (!line[0])
			continue;
		/* Parse authentication settings. */
		if (line[0] == '"') {
			token = apr_strtok(&line[2], psep, &last);
			if (token != NULL) {
				if (0 == apr_strnatcmp(token, "DISALLOW_REUSE")) {
					user_config->disallow_reuse = true;
				} else if (0 == apr_strnatcmp(token, "WINDOW_SIZE")) {
					token = apr_strtok(NULL, psep, &last);

					err_char = is_digit_str(token);
					if (err_char)
						ap_log_perror(APLOG_MARK,
							      APLOG_ERR,
							      0, pool,
							      "read_user_totp_config: window size value '%s' contains invalid character %c at line %d",
							      token, err_char,
							      line_no);
					else
						user_config->window_size =
						    max(0,
							min(apr_atoi64(token), 32));
				} else if (0 == apr_strnatcmp(token, "RATE_LIMIT")) {
					token = apr_strtok(NULL, psep, &last);

					err_char = is_digit_str(token);
					if (err_char)
						ap_log_perror(APLOG_MARK,
							      APLOG_ERR,
							      0, pool,
							      "read_user_totp_config: rate limit count value '%s' contains invalid character %c at line %d",
							      token, err_char,
							      line_no);
					else
						user_config->rate_limit_count =
						    max(0,
							min(apr_atoi64(token), 5));

					token = apr_strtok(NULL, psep, &last);

					err_char = is_digit_str(token);
					if (err_char) {
						user_config->rate_limit_count = 0;
						ap_log_perror(APLOG_MARK,
							      APLOG_ERR,
							      0, pool,
							      "read_user_totp_config: rate limit seconds value '%s' contains invalid character %c at line %d",
							      token, err_char,
							      line_no);
					} else
						user_config->rate_limit_seconds =
						    max(0,
							min(apr_atoi64(token), 300));
				} else
					ap_log_perror(APLOG_MARK, APLOG_DEBUG,
						      0, pool,
						      "read_user_totp_config: unrecognized directive \"%s\" at line %d",
						      line, line_no);

			} else
				ap_log_perror(APLOG_MARK, APLOG_DEBUG,
					      0, pool,
					      "read_user_totp_config: skipping comment line \"%s\" at line %d",
					      line, line_no);
		}
		/* Shared key is on the first valid line */
		else if (!user_config->shared_key) {
			token = apr_pstrdup(pool, line);
			line_len = strlen(token);

			user_config->shared_key = apr_pdecode_base32(pool, token, line_len, APR_ENCODE_NONE, &user_config->shared_key_len);

			if(!user_config->shared_key) {
				ap_log_perror(APLOG_MARK, APLOG_ERR, 0, pool,
					      "read_user_totp_config: could not find a valid BASE32 encoded secret at line %d",
						  line_no);
				return NULL;
			}
		}
		/* Handle scratch codes */
		else {
			token = apr_pstrdup(pool, line);
			line_len = strlen(token);

			/* validate scratch code */
			err_char = is_digit_str(token);
			if (err_char)
				ap_log_perror(APLOG_MARK, APLOG_ERR, 0, pool,
					      "read_user_totp_config: scratch code '%s' contains invalid character %c at line %d",
					      line, err_char, line_no);
			else if (user_config->scratch_codes_count < 10)
				user_config->
				    scratch_codes[user_config->scratch_codes_count++]
				    = apr_atoi64(token);
			else
				ap_log_perror(APLOG_MARK, APLOG_ERR, 0, pool,
					      "read_user_totp_config: scratch code '%s' at line %d was skipped, only 10 scratch codes per user are supported",
					      line, line_no);
		}
	}

	ap_cfg_closefile(config_file);

	return user_config;
}


/**
  * Update file entries and apend new entry
 **/
static apr_status_t
totp_check_n_update_file_helper(const char *filepath, const void *entry, apr_size_t entry_size,
			totp_file_helper_cb cb_check, totp_file_helper_cb_data *cb_data, apr_pool_t *pool)
{
	apr_status_t    status;
    const char     *tmp_filepath;
	apr_file_t     *tmp_file;
	apr_file_t     *target_file;
    apr_finfo_t     target_finfo;
    apr_mmap_t     *target_mmap;
    apr_size_t      bytes_written;
    apr_time_t      timestamp = *((apr_time_t *)entry);
    apr_size_t      entry_pos;
    apr_time_t      entry_time;
    const char     *file_data;

    tmp_filepath = apr_psprintf(pool, "%s.%" APR_TIME_T_FMT, filepath, timestamp);

	status = apr_file_open(&tmp_file,    /* temporary file handle */
			       tmp_filepath,         /* file name */
			       APR_FOPEN_EXCL     |  /* return an error if file exists */
			       APR_FOPEN_WRITE    |  /* open file for writing */
			       APR_FOPEN_CREATE   |  /* create file if it does
							              * not exist */
                   APR_FOPEN_BUFFERED |  /* buffered file IO */
			       APR_FOPEN_TRUNCATE,	 /* truncate file to 0 length */
			       APR_UREAD|APR_UWRITE, /* set read/write permissions 
				                          * only for owner */
			       pool	             /* memory pool to use */
	    );
	if (APR_SUCCESS != status) {
		ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
			      "totp_update_file_helper: could not create temporary file \"%s\"",
			      tmp_filepath);
		return status;
	}

	status = apr_file_open(&target_file, /* target file handle */
			       filepath,	         /* file name */
			       APR_FOPEN_READ,       /* open file for reading */
			       APR_FPROT_OS_DEFAULT, /* default permissions */
			       pool	             /* memory pool to use */
	    );
	if ((APR_SUCCESS != status) && (APR_ENOENT != status)) {
		ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
			      "totp_update_file_helper: could not open target file \"%s\"",
			      filepath);
		apr_file_close(tmp_file);
		return status;
	}

	if (APR_ENOENT != status) {
		/* Read current target file contents into a memory map */
		if ((status = apr_file_info_get(&target_finfo, APR_FINFO_SIZE, target_file)) != APR_SUCCESS) {
			ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
					"totp_update_file_helper: could not get target file \"%s\" size",
					filepath);
			apr_file_close(tmp_file);
			return status;
		}
		if ((status = apr_mmap_create(&target_mmap, target_file, 0, target_finfo.size, APR_MMAP_READ, pool)) != APR_SUCCESS) {
			ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
					"totp_update_file_helper: could not load target file \"%s\" into memory",
					filepath);
			apr_file_close(tmp_file);
			return status;
		}

		/* close the target file once contents have been loaded into memory */
		apr_file_close(target_file);

		/* process the file contents */
		file_data = target_mmap->mm;
		for(entry_pos = 0; entry_pos < target_mmap->size;
            entry_pos += entry_size, file_data += entry_size) {
			entry_time = *((apr_time_t*)file_data);

			if(timestamp >= entry_time) {
				/* check if entry time is within time tolerance */
				if((*cb_check) (entry, file_data, cb_data)) {
					/* keep the entry */
					ap_log_perror(APLOG_MARK, APLOG_DEBUG, 0, pool,
							"totp_update_file_helper: entry %ld is kept, cb_data->res = %u",
							entry_time, cb_data->res);
						
                    bytes_written = entry_size;
					if (((status = apr_file_write(tmp_file, file_data, &bytes_written)) != APR_SUCCESS) ||
                        (bytes_written != entry_size)) {
						ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
								"totp_update_file_helper: could not write to temporary file \"%s\"",
								tmp_filepath);
                        apr_mmap_delete(target_mmap);
						apr_file_close(tmp_file);
						return status;
					}
				} else {
					ap_log_[error(APLOG_MARK, APLOG_DEBUG, 0, pool,
							"totp_update_file_helper: entry %ld is NOT kept, cb_data->res = %u",
							entry_time, cb_data->res);
				}
			} else {
				/* entry is in the future */
				ap_log_perror(APLOG_MARK, APLOG_DEBUG, 0, pool,
						"totp_update_file_helper: entry %ld is in the future and will be dropped",
						entry_time);
			}
		}

		/* delete the memory map */
		apr_mmap_delete(target_mmap);
	}

    /* add current entry to file */
	if((*cb_check) (entry, NULL, cb_data)) {
		ap_log_perror(APLOG_MARK, APLOG_DEBUG, 0, pool,
				"totp_update_file_helper: adding new entry %ld, cb_data->res = %u",
				timestamp, cb_data->res);
		bytes_written = entry_size;
		if (((status = apr_file_write(tmp_file, entry, &bytes_written)) != APR_SUCCESS) ||
			(bytes_written != entry_size)) {
			ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
					"totp_update_file_helper: could not write to temporary file \"%s\"",
					tmp_filepath);
			apr_file_close(tmp_file);
			return status;
		}
	} else {
		ap_log_perror(APLOG_MARK, APLOG_DEBUG, 0, pool,
				"totp_update_file_helper: NOT adding new entry %ld, cb_data->res = %u",
				timestamp, cb_data->res);
	}

	apr_file_close(tmp_file);

	status = apr_file_rename(tmp_filepath, filepath, r->pool);
	if (APR_SUCCESS != status) {
		ap_log_perror(APLOG_MARK, APLOG_ERR, status, pool,
			      "totp_update_file_helper: unable to move file \"%s\" to \"%s\"",
			      tmp_filepath, filepath);
		return status;
	}

	return APR_SUCCESS;
}
