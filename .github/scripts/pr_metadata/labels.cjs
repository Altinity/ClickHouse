const versionLabels = new Map([
  ['releases/23.8.16', 'stable-23.8'],
  ['releases/24.3.18', 'stable-24.3'],
  ['releases/24.8.14', 'stable-24.8'],
  ['releases/25.3.14', 'stable-25.3'],
  ...[
    'stable-25.8', 'stable-26.3', 'stable-26.8',
    'antalya-25.8', 'antalya-26.3', 'antalya-26.6', 'antalya-26.8',
    'fips-25.3', 'fips-26.3',
  ].map(branch => [branch, branch]),
]);

module.exports = function labelsFor(base) {
  const version = versionLabels.get(base);
  return version ? [version.split('-')[0], version] : [];
};
