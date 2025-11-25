# RankingOptions


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**ranker** | **str** | The ranker to use for the file search. | [optional] 
**score_threshold** | **float** | The score threshold for the file search, a number between 0 and 1. Numbers closer to 1 will attempt to return only the most relevant results, but may return fewer results. | [optional] 

## Example

```python
from openapi_client.models.ranking_options import RankingOptions

# TODO update the JSON string below
json = "{}"
# create an instance of RankingOptions from a JSON string
ranking_options_instance = RankingOptions.from_json(json)
# print the JSON string representation of the object
print(RankingOptions.to_json())

# convert the object into a dict
ranking_options_dict = ranking_options_instance.to_dict()
# create an instance of RankingOptions from a dict
ranking_options_from_dict = RankingOptions.from_dict(ranking_options_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


