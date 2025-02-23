<?php
/**
 * @file
 * Sitemap generation and rebuilding functions for the xmlsitemap module.
 *
 * @ingroup xmlsitemap
 */

/**
 * Given an internal path, return the alias for the path.
 *
 * This is similar to backdrop_get_path_alias(), but designed to fetch all alises
 * at once so that only one database query is executed instead of several or
 * possibly thousands during sitemap generation.
 *
 * @param string $path
 *   An internal Backdrop path.
 * @param string $language
 *   A language code to use when looking up the paths.
 */
function xmlsitemap_get_path_alias($path, $language) {
  static $aliases;
  static $last_language;

  if (!isset($aliases)) {
    $aliases[LANGUAGE_NONE] = db_query("SELECT source, alias FROM {url_alias} WHERE langcode = :language ORDER BY pid", array(':language' => LANGUAGE_NONE))->fetchAllKeyed();
  }
  if ($language != LANGUAGE_NONE && $last_language != $language) {
    unset($aliases[$last_language]);
    $aliases[$language] = db_query("SELECT source, alias FROM {url_alias} WHERE langcode = :language ORDER BY pid", array(':language' => $language))->fetchAllKeyed();
    $last_language = $language;
  }

  // We need to pass our path through hook_url_outbound_alter(). This fixes
  // clean URLs not working when they don't exist in the {url_alias} table and
  // are created with something like subpathauto.
  $normalized_path = $path;

  // hook_url_outbound_alter() expects defaults in url() options.
  $options = array(
    'fragment' => '',
    'query' => array(),
    'absolute' => FALSE,
    'alias' => FALSE,
    'prefix' => '',
    'external' => FALSE,
  );

  if ($language != LANGUAGE_NONE && isset($aliases[$language][$path])) {
    $normalized_path = $aliases[$language][$path];
    $options['alias'] = TRUE;
  }
  elseif (isset($aliases[LANGUAGE_NONE][$path])) {
    $normalized_path = $aliases[LANGUAGE_NONE][$path];
    $options['alias'] = TRUE;
  }

  $original_path = $normalized_path;
  backdrop_alter('url_outbound', $normalized_path, $options, $original_path);
  return $normalized_path;
}

/**
 * Perform operations before rebuilding the sitemap.
 */
function _xmlsitemap_regenerate_before() {
  // Attempt to increase the memory limit.
  _xmlsitemap_set_memory_limit();

  if (config_get('xmlsitemap.settings', 'developer_mode', 0)) {
    watchdog('xmlsitemap', 'Starting XML sitemap generation. Memory usage: @memory-peak.', array(
      '@memory-peak' => format_size(memory_get_peak_usage(TRUE)),
    ),
      WATCHDOG_DEBUG
    );
  }
}

/**
 * Get Memory Usage.
 */
function _xmlsitemap_get_memory_usage($start = FALSE) {
  static $memory_start;
  $current = memory_get_peak_usage(TRUE);
  if (!isset($memory_start) || $start) {
    $memory_start = $current;
  }
  return $current - $memory_start;
}

/**
 * Calculate the optimal PHP memory limit for sitemap generation.
 *
 * This function just makes a guess. It does not take into account
 * the currently loaded modules.
 */
function _xmlsitemap_get_optimal_memory_limit() {
  $optimal_limit = &backdrop_static(__FUNCTION__);
  if (!isset($optimal_limit)) {
    // Set the base memory amount from the provided core constant.
    $optimal_limit = parse_size(BACKDROP_MINIMUM_PHP_MEMORY_LIMIT);

    // Add memory based on the chunk size.
    $optimal_limit += xmlsitemap_get_chunk_size() * 500;

    // Add memory for storing the url aliases.
    if (config_get('xmlsitemap.settings', 'prefetch_aliases', 1)) {
      $aliases = db_query("SELECT COUNT(pid) FROM {url_alias}")->fetchField();
      $optimal_limit += $aliases * 250;
    }
  }
  return $optimal_limit;
}

/**
 * Calculate the optimal memory level for sitemap generation.
 *
 * @param string $new_limit
 *   An optional PHP memory limit in bytes. If not provided, the value of
 *   _xmlsitemap_get_optimal_memory_limit() will be used.
 */
function _xmlsitemap_set_memory_limit($new_limit = NULL) {
  $current_limit = @ini_get('memory_limit');
  if ($current_limit && $current_limit != -1) {
    if (!is_null($new_limit)) {
      $new_limit = _xmlsitemap_get_optimal_memory_limit();
    }
    if (parse_size($current_limit) < $new_limit) {
      return @ini_set('memory_limit', $new_limit);
    }
  }
}

/**
 * Generate one page (chunk) of the sitemap.
 *
 * @param object $sitemap
 *   An unserialized data array for an XML sitemap.
 * @param string $page
 *   An integer of the specific page of the sitemap to generate.
 */
function xmlsitemap_generate_page(stdClass $sitemap, $page) {
  try {
    $writer = new XMLSitemapWriter($sitemap, $page);
    $writer->startDocument();
    $writer->generateXML();
    $writer->endDocument();
  }
  catch (Exception $e) {
    watchdog_exception('xmlsitemap', $e);
    throw $e;
  }

  return $writer->getSitemapElementCount();
}

/**
 * Generate chunk.
 */
function xmlsitemap_generate_chunk(stdClass $sitemap, XMLSitemapWriter $writer, $chunk) {
  $config = config('xmlsitemap.settings');
  $base_url = !empty($config->get('base_url')) ? $config->get('base_url') : $GLOBALS['base_url'];
  $output_elements = backdrop_map_assoc($config->get('output_elements'));
  $lastmod_format = $config->get('lastmod_format');

  $url_options = $sitemap->uri['options'];
  $url_options += array(
    'absolute' => TRUE,
    'base_url' => $base_url,
    'language' => language_default(),
    'alias' => $config->get('prefetch_aliases', TRUE),
  );

  $last_url = '';
  $link_count = 0;

  $query = db_select('xmlsitemap', 'x');
  $query->fields('x', array(
    'id',
    'type',
    'subtype',
    'loc',
    'lastmod',
    'changefreq',
    'changecount',
    'priority',
    'language',
    'access',
    'status',
  ));
  $query->condition('x.access', 1);
  $query->condition('x.status', 1);
  $query->orderBy('x.language', 'DESC');
  $query->orderBy('x.loc');
  $query->addTag('xmlsitemap_generate');
  $query->addMetaData('sitemap', $sitemap);

  $offset = max($chunk - 1, 0) * xmlsitemap_get_chunk_size();
  $limit = xmlsitemap_get_chunk_size();
  $query->range($offset, $limit);
  $links = $query->execute();

  while ($link = $links->fetchAssoc()) {
    $link['language'] = $link['language'] != LANGUAGE_NONE ? xmlsitemap_language_load($link['language']) : $url_options['language'];
    $parsed_url = backdrop_parse_url($link['loc']);
    // Skip nodes which are 301 redirected.
    if (variable_get('xmlsitemap_redirect')) {
      $relative_redirect = redirect_fetch_rids_by_path($link['loc'], $link['language']->language, TRUE);
      $alias_redirect = redirect_fetch_rids_by_path(ltrim(url($link['loc']), '/'), $link['language']->language, TRUE);
      // If node contains a 301 redirect we skip it.
      if (!empty($relative_redirect) || !empty($alias_redirect)) {
        continue;
      }
    }
    // Remove query or fragment.
    $link['loc'] = $parsed_url['path'];
    if ($url_options['alias']) {
      $link['loc'] = xmlsitemap_get_path_alias($link['loc'], $link['language']->langcode);
    }
    $link_options = array(
      'language' => $link['language'],
      'xmlsitemap_link' => $link,
      'xmlsitemap_sitemap' => $sitemap,
      'query' => $parsed_url['query'],
      'fragment' => $parsed_url['fragment'],
    );
    // @todo Add a separate hook_xmlsitemap_link_url_alter() here?
    $link_url = url($link['loc'], $link_options + $url_options);

    // Skip this link if it was a duplicate of the last one.
    // @todo Figure out a way to do this before generation so we can report
    // back to the user about this.
    if ($link_url == $last_url) {
      continue;
    }
    else {
      $last_url = $link_url;
      // Keep track of the total number of links written.
      $link_count++;
    }

    $element = array();
    $element['loc'] = urldecode($link_url);
    if ($link['lastmod']) {
      if (!empty($output_elements['lastmod'])) {
        $element['lastmod'] = gmdate($lastmod_format, $link['lastmod']);
      }
      // If the link has a lastmod value, update the changefreq so that links
      // with a short changefreq but updated two years ago show decay.
      // We use abs() here just incase items were created on this same cron run
      // because lastmod would be greater than REQUEST_TIME.
      $link['changefreq'] = (abs(REQUEST_TIME - $link['lastmod']) + $link['changefreq']) / 2;
    }
    if (!empty($output_elements['changefreq']) && $link['changefreq']) {
      $element['changefreq'] = xmlsitemap_get_changefreq($link['changefreq']);
    }
    if (!empty($output_elements['priority']) && isset($link['priority']) && $link['priority'] != 0.5) {
      // Don't output the priority value for links that have 0.5 priority. This
      // is the default 'assumed' value if priority is not included as per the
      // sitemaps.org specification.
      $element['priority'] = number_format($link['priority'], 1);
    }

    // @todo Should this be moved to XMLSitemapWritier::writeSitemapElement()?
    backdrop_alter('xmlsitemap_element', $element, $link, $sitemap);

    if (!empty($element)) {
      $writer->writeSitemapElement('url', $element);
    }
  }

  return $link_count;
}

/**
 * Generate the index sitemap.
 *
 * @param object $sitemap
 *   An unserialized data array for an XML sitemap.
 */
function xmlsitemap_generate_index(stdClass $sitemap) {
  try {
    $writer = new XMLSitemapIndexWriter($sitemap);
    $writer->startDocument();
    $writer->generateXML();
    $writer->endDocument();
  }
  catch (Exception $e) {
    watchdog_exception('xmlsitemap', $e);
    throw $e;
  }

  return $writer->getSitemapElementCount();
}

/**
 * BATCH OPERATIONS -----------------------------------------------------------.
 *
 * Batch information callback for regenerating the sitemap files.
 *
 * @param array $smids
 *   An optional array of XML sitemap IDs. If not provided, it will load all
 *   existing XML sitemaps.
 */
function xmlsitemap_regenerate_batch(array $smids = array()) {
  if (empty($smids)) {
    $smids = db_query("SELECT smid FROM {xmlsitemap_sitemap}")->fetchCol();
  }
  $batch = array(
    'operations' => array(),
    'finished' => 'xmlsitemap_regenerate_batch_finished',
    'title' => t('Regenerating Sitemap'),
    'file' => backdrop_get_path('module', 'xmlsitemap') . '/xmlsitemap.generate.inc',
  );

  // Set the regenerate flag in case something fails during file generation.
  $batch['operations'][] = array('xmlsitemap_batch_state_set', array(array('xmlsitemap_regenerate_needed' => TRUE)));

  // @todo Get rid of this batch operation.
  $batch['operations'][] = array('_xmlsitemap_regenerate_before', array());

  // Generate all the sitemap pages for each context.
  foreach ($smids as $smid) {
    $batch['operations'][] = array('xmlsitemap_regenerate_batch_generate', array($smid));
    $batch['operations'][] = array('xmlsitemap_regenerate_batch_generate_index', array($smid));
  }

  // Clear the regeneration flag.
  $batch['operations'][] = array('xmlsitemap_batch_state_set', array(array('xmlsitemap_regenerate_needed' => FALSE)));

  return $batch;
}

/**
 * Batch callback; generate all pages of a sitemap.
 */
function xmlsitemap_regenerate_batch_generate($smid, array &$context) {
  if (!isset($context['sandbox']['sitemap'])) {
    $context['sandbox']['sitemap'] = xmlsitemap_sitemap_load($smid);
    $context['sandbox']['sitemap']->chunks = 1;
    $context['sandbox']['sitemap']->links = 0;
    $context['sandbox']['max'] = XMLSITEMAP_MAX_SITEMAP_LINKS;

    // Clear the cache directory for this sitemap before generating any files.
    xmlsitemap_check_directory($context['sandbox']['sitemap']);
    xmlsitemap_clear_directory($context['sandbox']['sitemap']);
  }

  $sitemap = &$context['sandbox']['sitemap'];
  $links = xmlsitemap_generate_page($sitemap, $sitemap->chunks);
  $context['message'] = t('Now generating %sitemap-url.', array('%sitemap-url' => url('sitemap.xml', $sitemap->uri['options'] + array('query' => array('page' => $sitemap->chunks)))));

  if ($links) {
    $sitemap->links += $links;
    $sitemap->chunks++;
  }
  else {
    // Cleanup the 'extra' empty file.
    $file = xmlsitemap_sitemap_get_file($sitemap, $sitemap->chunks);
    if (file_exists($file) && $sitemap->chunks > 1) {
      file_unmanaged_delete($file);
    }
    $sitemap->chunks--;

    // Save the updated chunks and links values.
    $context['sandbox']['max'] = $sitemap->chunks;
    $sitemap->updated = REQUEST_TIME;
    xmlsitemap_sitemap_get_max_filesize($sitemap);
    xmlsitemap_sitemap_save($sitemap);
  }

  if ($sitemap->chunks != $context['sandbox']['max']) {
    $context['finished'] = $sitemap->chunks / $context['sandbox']['max'];
  }
}

/**
 * Batch callback; generate the index page of a sitemap.
 */
function xmlsitemap_regenerate_batch_generate_index($smid, array &$context) {
  $sitemap = xmlsitemap_sitemap_load($smid);
  if ($sitemap->chunks > 1) {
    xmlsitemap_generate_index($sitemap);
    $context['message'] = t('Now generating sitemap index %sitemap-url.', array('%sitemap-url' => url('sitemap.xml', $sitemap->uri['options'])));
  }
}

/**
 * Batch callback; sitemap regeneration finished.
 */
function xmlsitemap_regenerate_batch_finished($success, $results, $operations, $elapsed) {
  if ($success && !state_get('xmlsitemap_regenerate_needed', FALSE)) {
    state_set('xmlsitemap_generated_last', REQUEST_TIME);

    // Show a watchdog message that the sitemap was regenerated.
    watchdog('xmlsitemap',
      'Finished XML sitemap generation in @elapsed. Memory usage: @memory-peak.',
      array(
        '@elapsed' => $elapsed,
        '@memory-peak' => format_size(memory_get_peak_usage(TRUE)),
      ),
      WATCHDOG_NOTICE
    );
    module_invoke_all('xmlsitemap_regenerate_finished');
  }
  else {
    backdrop_set_message(t('The sitemaps were not successfully regenerated.'), 'error');
  }
}

/**
 * Batch information callback for rebuilding the sitemap data.
 */
function xmlsitemap_rebuild_batch(array $entities, $save_custom = FALSE) {
  $batch = array(
    'operations' => array(),
    'finished' => 'xmlsitemap_rebuild_batch_finished',
    'title' => t('Rebuilding Sitemap'),
    'file' => backdrop_get_path('module', 'xmlsitemap') . '/xmlsitemap.generate.inc',
  );

  // Set the rebuild flag in case something fails during the rebuild.
  $batch['operations'][] = array('xmlsitemap_batch_state_set', array(array('xmlsitemap_rebuild_needed' => TRUE)));

  // Purge any links first.
  $batch['operations'][] = array('xmlsitemap_rebuild_batch_clear',
    array($entities, (bool) $save_custom),
  );

  // Fetch all the sitemap links and save them to the {xmlsitemap} table.
  foreach ($entities as $entity) {
    $info = xmlsitemap_get_link_info($entity);
    $batch['operations'][] = array($info['xmlsitemap']['rebuild callback'], array($entity));
  }

  // Clear the rebuild flag.
  $batch['operations'][] = array('xmlsitemap_batch_state_set', array(array('xmlsitemap_rebuild_needed' => FALSE)));

  // Add the regeneration batch.
  $regenerate_batch = xmlsitemap_regenerate_batch();
  $batch['operations'] = array_merge($batch['operations'], $regenerate_batch['operations']);

  return $batch;
}

/**
 * Batch callback; set an array of variables and their values.
 */
function xmlsitemap_batch_state_set(array $variables) {
  foreach ($variables as $variable => $value) {
    state_set($variable, $value);
  }
}

/**
 * Batch callback; clear sitemap links for entites.
 */
function xmlsitemap_rebuild_batch_clear(array $entities, $save_custom, &$context) {
  if (!empty($entities)) {
    xmlsitemap_rebuild_clear($entities, $save_custom);
  }
  $context['message'] = t('Purging links.');
}

/**
 * Batch callback; fetch and add the sitemap links for a specific entity.
 */
function xmlsitemap_rebuild_batch_fetch($entity, &$context) {
  if (!isset($context['sandbox']['info'])) {
    $context['sandbox']['info'] = xmlsitemap_get_link_info($entity);
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['last_id'] = 0;
  }
  $info = $context['sandbox']['info'];

  $query = new EntityFieldQuery();
  $query->entityCondition('entity_type', $entity);
  $query->entityCondition('entity_id', $context['sandbox']['last_id'], '>');
  $query->addTag('xmlsitemap_link_bundle_access');
  $query->addTag('xmlsitemap_rebuild');
  $query->addMetaData('entity', $entity);
  $query->addMetaData('entity_info', $info);
  if ($types = xmlsitemap_get_link_type_enabled_bundles($entity)) {
    $query->entityCondition('bundle', $types, 'IN');
  }
  else {
    // If no enabled bundle types, skip everything else.
    return;
  }

  if (!isset($context['sandbox']['max'])) {
    $count_query = clone $query;
    $count_query->count();
    $context['sandbox']['max'] = $count_query->execute();
    if (!$context['sandbox']['max']) {
      // If there are no items to process, skip everything else.
      return;
    }
  }

  // PostgreSQL cannot have the ORDERED BY in the count query.
  $query->entityOrderBy('entity_id');
  $limit = 20;
  $query->range(0, $limit);

  $result = $query->execute();
  $ids = array_keys($result[$entity]);

  $info['xmlsitemap']['process callback']($ids);
  $context['sandbox']['last_id'] = end($ids);
  $context['sandbox']['progress'] += count($ids);
  $context['message'] = t('Now processing %entity @last_id (@progress of @count).', array(
    '%entity' => $entity,
    '@last_id' => $context['sandbox']['last_id'],
    '@progress' => $context['sandbox']['progress'],
    '@count' => $context['sandbox']['max'],
  ));

  if ($context['sandbox']['progress'] >= $context['sandbox']['max']) {
    $context['finished'] = 1;
  }
  else {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

/**
 * Batch callback; sitemap rebuild finished.
 */
function xmlsitemap_rebuild_batch_finished($success, $results, $operations, $elapsed) {
  if ($success && !state_get('xmlsitemap_rebuild_needed', FALSE)) {
    backdrop_set_message(t('The sitemap links were rebuilt.'));
  }
  else {
    backdrop_set_message(t('The sitemap links were not successfully rebuilt.'), 'error');
  }
}

/**
 * Get Rebuildable link types.
 */
function xmlsitemap_get_rebuildable_link_types() {
  $rebuild_types = array();
  $entities = xmlsitemap_get_link_info();

  foreach ($entities as $entity => $info) {
    if (empty($info['xmlsitemap']['rebuild callback'])) {
      // If the entity is missing a rebuild callback, skip.
      continue;
    }
    if (!empty($info['entity keys']['bundle']) && !xmlsitemap_get_link_type_enabled_bundles($entity)) {
      // If the entity has bundles, but no enabled bundles, skip since
      // rebuilding wouldn't get any links.
      continue;
    }
    else {
      $rebuild_types[] = $entity;
    }
  }

  return $rebuild_types;
}

/**
 * Clear all sitemap links for given entity types.
 *
 * @param array $types
 *   An array of link types.
 * @param bool $save_custom
 *   A boolean if links with status or priority overridden should not be
 *   removed (and hence overridden values not lost).
 *
 * @return int
 *   The number of deleted links.
 */
function xmlsitemap_rebuild_clear(array $types, $save_custom) {
  // Let other modules respond to the rebuild clearing.
  module_invoke_all('xmlsitemap_rebuild_clear', $types, $save_custom);

  $query = db_delete('xmlsitemap');
  $query->condition('type', $types);

  // If we want to save the custom data, make sure to exclude any links
  // that are not using default inclusion or priority.
  if ($save_custom) {
    $query->condition('status_override', 0);
    $query->condition('priority_override', 0);
  }

  return $query->execute();
}
